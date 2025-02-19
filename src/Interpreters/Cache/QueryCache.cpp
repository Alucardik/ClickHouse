#include "Interpreters/Cache/QueryCache.h"

#include <Functions/FunctionFactory.h>
#include <Interpreters/Context.h>
#include <Interpreters/InDepthNodeVisitor.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/IAST.h>
#include <Processors/Sources/SourceFromChunks.h>
#include <Common/logger_useful.h>
#include <Common/ProfileEvents.h>
#include <Common/SipHash.h>
#include <Common/TTLCachePolicy.h>
#include <Core/Settings.h>
#include <base/defines.h> /// chassert


namespace ProfileEvents
{
    extern const Event QueryCacheHits;
    extern const Event QueryCacheMisses;
};

namespace DB
{

namespace
{

struct HasNonDeterministicFunctionsMatcher
{
    struct Data
    {
        const ContextPtr context;
        bool has_non_deterministic_functions = false;
    };

    static bool needChildVisit(const ASTPtr &, const ASTPtr &) { return true; }

    static void visit(const ASTPtr & node, Data & data)
    {
        if (data.has_non_deterministic_functions)
            return;

        if (const auto * function = node->as<ASTFunction>())
        {
            const auto func = FunctionFactory::instance().tryGet(function->name, data.context);
            if (func && !func->isDeterministic())
                data.has_non_deterministic_functions = true;
        }
    }
};

using HasNonDeterministicFunctionsVisitor = InDepthNodeVisitor<HasNonDeterministicFunctionsMatcher, true>;

}

bool astContainsNonDeterministicFunctions(ASTPtr ast, ContextPtr context)
{
    HasNonDeterministicFunctionsMatcher::Data finder_data{context};
    HasNonDeterministicFunctionsVisitor(finder_data).visit(ast);
    return finder_data.has_non_deterministic_functions;
}

namespace
{

class RemoveQueryCacheSettingsMatcher
{
public:
    struct Data {};

    static bool needChildVisit(ASTPtr &, const ASTPtr &) { return true; }

    static void visit(ASTPtr & ast, Data &)
    {
        if (auto * set_clause = ast->as<ASTSetQuery>())
        {
            chassert(!set_clause->is_standalone);

            auto is_query_cache_related_setting = [](const auto & change)
            {
                return change.name == "allow_experimental_query_cache"
                    || change.name.starts_with("query_cache")
                    || change.name.ends_with("query_cache");
            };

            std::erase_if(set_clause->changes, is_query_cache_related_setting);
        }
    }

    /// TODO further improve AST cleanup, e.g. remove SETTINGS clause completely if it is empty
    /// E.g. SELECT 1 SETTINGS use_query_cache = true
    /// and  SELECT 1;
    /// currently don't match.
};

using RemoveQueryCacheSettingsVisitor = InDepthNodeVisitor<RemoveQueryCacheSettingsMatcher, true>;

/// Consider
///   (1) SET use_query_cache = true;
///       SELECT expensiveComputation(...) SETTINGS max_threads = 64, query_cache_ttl = 300;
///       SET use_query_cache = false;
/// and
///   (2) SELECT expensiveComputation(...) SETTINGS max_threads = 64, use_query_cache = true;
///
/// The SELECT queries in (1) and (2) are basically the same and the user expects that the second invocation is served from the query
/// cache. However, query results are indexed by their query ASTs and therefore no result will be found. Insert and retrieval behave overall
/// more natural if settings related to the query cache are erased from the AST key. Note that at this point the settings themselves
/// have been parsed already, they are not lost or discarded.
ASTPtr removeQueryCacheSettings(ASTPtr ast)
{
    ASTPtr transformed_ast = ast->clone();

    RemoveQueryCacheSettingsMatcher::Data visitor_data;
    RemoveQueryCacheSettingsVisitor(visitor_data).visit(transformed_ast);

    return transformed_ast;
}

}

QueryCache::Key::Key(
    ASTPtr ast_,
    Block header_,
    const std::optional<String> & username_,
    std::chrono::time_point<std::chrono::system_clock> expires_at_,
    bool is_compressed_)
    : ast(removeQueryCacheSettings(ast_))
    , header(header_)
    , username(username_)
    , expires_at(expires_at_)
    , is_compressed(is_compressed_)
{
}

bool QueryCache::Key::operator==(const Key & other) const
{
    return ast->getTreeHash() == other.ast->getTreeHash();
}

String QueryCache::Key::queryStringFromAst() const
{
    WriteBufferFromOwnString buf;
    IAST::FormatSettings format_settings(buf, /*one_line*/ true);
    format_settings.show_secrets = false;
    ast->format(format_settings);
    return buf.str();
}

size_t QueryCache::KeyHasher::operator()(const Key & key) const
{
    SipHash hash;
    hash.update(key.ast->getTreeHash());
    auto res = hash.get64();
    return res;
}

size_t QueryCache::QueryResultWeight::operator()(const Chunks & chunks) const
{
    size_t res = 0;
    for (const auto & chunk : chunks)
        res += chunk.allocatedBytes();
    return res;
}

bool QueryCache::IsStale::operator()(const Key & key) const
{
    return (key.expires_at < std::chrono::system_clock::now());
};

QueryCache::Writer::Writer(Cache & cache_, const Key & key_,
    size_t max_entry_size_in_bytes_, size_t max_entry_size_in_rows_,
    std::chrono::milliseconds min_query_runtime_,
    bool squash_partial_results_,
    size_t max_block_size_)
    : cache(cache_)
    , key(key_)
    , max_entry_size_in_bytes(max_entry_size_in_bytes_)
    , max_entry_size_in_rows(max_entry_size_in_rows_)
    , min_query_runtime(min_query_runtime_)
    , squash_partial_results(squash_partial_results_)
    , max_block_size(max_block_size_)
{
    if (auto entry = cache.getWithKey(key); entry.has_value() && !IsStale()(entry->key))
    {
        skip_insert = true; /// Key already contained in cache and did not expire yet --> don't replace it
        LOG_TRACE(&Poco::Logger::get("QueryCache"), "Skipped insert (non-stale entry found), query: {}", key.queryStringFromAst());
    }
}

void QueryCache::Writer::buffer(Chunk && partial_query_result)
{
    if (skip_insert)
        return;

    std::lock_guard lock(mutex);

    auto & chunks = *query_result;

    chunks.emplace_back(std::move(partial_query_result));

    new_entry_size_in_bytes += chunks.back().allocatedBytes();
    new_entry_size_in_rows += chunks.back().getNumRows();

    if ((new_entry_size_in_bytes > max_entry_size_in_bytes) || (new_entry_size_in_rows > max_entry_size_in_rows))
    {
        chunks.clear(); /// eagerly free some space
        skip_insert = true;
        LOG_TRACE(&Poco::Logger::get("QueryCache"), "Skipped insert (query result too big), new_entry_size_in_bytes: {} ({}), new_entry_size_in_rows: {} ({}), query: {}", new_entry_size_in_bytes, max_entry_size_in_bytes, new_entry_size_in_rows, max_entry_size_in_rows, key.queryStringFromAst());
    }
}

void QueryCache::Writer::finalizeWrite()
{
    if (skip_insert)
        return;

    std::lock_guard lock(mutex);

    chassert(!was_finalized);

    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - query_start_time) < min_query_runtime)
    {
        LOG_TRACE(&Poco::Logger::get("QueryCache"), "Skipped insert (query not expensive enough), query: {}", key.queryStringFromAst());
        return;
    }

    if (auto entry = cache.getWithKey(key); entry.has_value() && !IsStale()(entry->key))
    {
        /// same check as in ctor because a parallel Writer could have inserted the current key in the meantime
        LOG_TRACE(&Poco::Logger::get("QueryCache"), "Skipped insert (non-stale entry found), query: {}", key.queryStringFromAst());
        return;
    }

    if (squash_partial_results)
    {
        // Squash partial result chunks to chunks of size 'max_block_size' each. This costs some performance but provides a more natural
        // compression of neither too small nor big blocks. Also, it will look like 'max_block_size' is respected when the query result is
        // served later on from the query cache.

        Chunks squashed_chunks;
        size_t rows_remaining_in_squashed = 0; /// how many further rows can the last squashed chunk consume until it reaches max_block_size

        for (auto & chunk : *query_result)
        {
            convertToFullIfSparse(chunk);

            const size_t rows_chunk = chunk.getNumRows();
            if (rows_chunk == 0)
                continue;

            size_t rows_chunk_processed = 0;
            while (true)
            {
                if (rows_remaining_in_squashed == 0)
                {
                    Chunk empty_chunk = Chunk(chunk.cloneEmptyColumns(), 0);
                    squashed_chunks.push_back(std::move(empty_chunk));
                    rows_remaining_in_squashed = max_block_size;
                }

                const size_t rows_to_append = std::min(rows_chunk - rows_chunk_processed, rows_remaining_in_squashed);
                squashed_chunks.back().append(chunk, rows_chunk_processed, rows_to_append);
                rows_chunk_processed += rows_to_append;
                rows_remaining_in_squashed -= rows_to_append;

                if (rows_chunk_processed == rows_chunk)
                    break;
            }
        }

        *query_result = std::move(squashed_chunks);
    }

    if (key.is_compressed)
    {
        Chunks compressed_chunks;
        const Chunks & decompressed_chunks = *query_result;
        for (const auto & decompressed_chunk : decompressed_chunks)
        {
            const Columns & decompressed_columns = decompressed_chunk.getColumns();
            Columns compressed_columns;
            for (const auto & decompressed_column : decompressed_columns)
            {
                auto compressed_column = decompressed_column->compress();
                compressed_columns.push_back(compressed_column);
            }
            Chunk compressed_chunk(compressed_columns, decompressed_chunk.getNumRows());
            compressed_chunks.push_back(std::move(compressed_chunk));
        }
        *query_result = std::move(compressed_chunks);
    }

    cache.set(key, query_result);

    was_finalized = true;
}

QueryCache::Reader::Reader(Cache & cache_, const Key & key, const std::lock_guard<std::mutex> &)
{
    auto entry = cache_.getWithKey(key);

    if (!entry.has_value())
    {
        LOG_TRACE(&Poco::Logger::get("QueryCache"), "No entry found for query {}", key.queryStringFromAst());
        return;
    }

    if (entry->key.username.has_value() && entry->key.username != key.username)
    {
        LOG_TRACE(&Poco::Logger::get("QueryCache"), "Inaccessible entry found for query {}", key.queryStringFromAst());
        return;
    }

    if (IsStale()(entry->key))
    {
        LOG_TRACE(&Poco::Logger::get("QueryCache"), "Stale entry found for query {}", key.queryStringFromAst());
        return;
    }

    if (!entry->key.is_compressed)
        pipe = Pipe(std::make_shared<SourceFromChunks>(entry->key.header, entry->mapped));
    else
    {
        auto decompressed_chunks = std::make_shared<Chunks>();
        const Chunks & compressed_chunks = *entry->mapped;
        for (const auto & compressed_chunk : compressed_chunks)
        {
            const Columns & compressed_chunk_columns = compressed_chunk.getColumns();
            Columns decompressed_columns;
            for (const auto & compressed_column : compressed_chunk_columns)
            {
                auto column = compressed_column->decompress();
                decompressed_columns.push_back(column);
            }
            Chunk decompressed_chunk(decompressed_columns, compressed_chunk.getNumRows());
            decompressed_chunks->push_back(std::move(decompressed_chunk));
        }

        pipe = Pipe(std::make_shared<SourceFromChunks>(entry->key.header, decompressed_chunks));
    }

    LOG_TRACE(&Poco::Logger::get("QueryCache"), "Entry found for query {}", key.queryStringFromAst());
}

bool QueryCache::Reader::hasCacheEntryForKey() const
{
    bool res = !pipe.empty();

    if (res)
        ProfileEvents::increment(ProfileEvents::QueryCacheHits);
    else
        ProfileEvents::increment(ProfileEvents::QueryCacheMisses);

    return res;
}

Pipe && QueryCache::Reader::getPipe()
{
    chassert(!pipe.empty()); // cf. hasCacheEntryForKey()
    return std::move(pipe);
}

QueryCache::Reader QueryCache::createReader(const Key & key)
{
    std::lock_guard lock(mutex);
    return Reader(cache, key, lock);
}

QueryCache::Writer QueryCache::createWriter(const Key & key, std::chrono::milliseconds min_query_runtime, bool squash_partial_results, size_t max_block_size)
{
    std::lock_guard lock(mutex);
    return Writer(cache, key, max_entry_size_in_bytes, max_entry_size_in_rows, min_query_runtime, squash_partial_results, max_block_size);
}

void QueryCache::reset()
{
    cache.reset();
    std::lock_guard lock(mutex);
    times_executed.clear();
    cache_size_in_bytes = 0;
}

size_t QueryCache::recordQueryRun(const Key & key)
{
    std::lock_guard lock(mutex);
    size_t times = ++times_executed[key];
    // Regularly drop times_executed to avoid DOS-by-unlimited-growth.
    static constexpr size_t TIMES_EXECUTED_MAX_SIZE = 10'000;
    if (times_executed.size() > TIMES_EXECUTED_MAX_SIZE)
        times_executed.clear();
    return times;
}

std::vector<QueryCache::Cache::KeyMapped> QueryCache::dump() const
{
    return cache.dump();
}

QueryCache::QueryCache()
    : cache(std::make_unique<TTLCachePolicy<Key, Chunks, KeyHasher, QueryResultWeight, IsStale>>())
{
}

void QueryCache::updateConfiguration(const Poco::Util::AbstractConfiguration & config)
{
    std::lock_guard lock(mutex);

    size_t max_size_in_bytes = config.getUInt64("query_cache.max_size", 1_GiB);
    cache.setMaxSize(max_size_in_bytes);

    size_t max_entries = config.getUInt64("query_cache.max_entries", 1024);
    cache.setMaxCount(max_entries);

    max_entry_size_in_bytes = config.getUInt64("query_cache.max_entry_size", 1_MiB);
    max_entry_size_in_rows = config.getUInt64("query_cache.max_entry_rows", 30'000'000);
}

}
