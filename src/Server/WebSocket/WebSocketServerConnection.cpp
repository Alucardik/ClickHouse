#include <Server/WebSocket/WebSocketServerConnection.h>

#include <Poco/Util/ServerApplication.h>
#include <Poco/JSON/Object.h>


namespace DB
{

void WebSocketServerConnection::run()
{
    using Poco::Util::Application;
    using Poco::JSON::Object;

    int flags_and_opcode = 0;
    int received_bytes = -1;

    Application& app = Application::instance();

    while (received_bytes != 0 && (flags_and_opcode & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
    {

        try {
            received_bytes = webSocket.receiveFrame(frame_buffer, flags_and_opcode);
        } catch (const Exception& e) {
            //TODO: add a reasonable exception wrapper here
            throw Exception(e);
        }
        auto opcode = flags_and_opcode & WebSocket::FRAME_OP_BITMASK;
        auto flag = flags_and_opcode & WebSocket::FRAME_FLAG_BITMASK;


        auto str1 = std::string(message_buffer.begin(), message_buffer.end());

        app.logger().information(
            Poco::format("Frame received (length=%d, flags=0x%x, op_flags=0x%x, frame_flags=0x%x). \n message:%s",
             received_bytes,
             unsigned(flags_and_opcode),
             unsigned(opcode),
             unsigned(flag),
             str1
        ));

        switch (opcode) {
            case WebSocket::FRAME_OP_CONT:
            case WebSocket::FRAME_OP_TEXT:
                message_buffer.append(frame_buffer);
                break;

            case WebSocket::FRAME_OP_PING:
            case WebSocket::FRAME_OP_CLOSE:
                message_buffer.assign(frame_buffer.begin(), frame_buffer.size());
                try {
                    std::string request(message_buffer.begin());
                    control_frames_handler.handleRequest(request, webSocket);
                } catch (const Exception& e) {
                    //TODO: add a reasonable exception wrapper here
                    throw Exception(e);
                }
                break;

            default:
                throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Binary data processing is not implemented yet");
        }

        if (flag == WebSocket::FRAME_FLAG_FIN) {
            try {
                // TODO: parse actual request JSON
                auto str = std::string(message_buffer.begin(), message_buffer.end());
                auto request = parser.parse(str).extract<Object::Ptr>();
                regular_handler.handleRequest(*request, webSocket);
                message_buffer.setCapacity(0);
            } catch (const Exception& e) {
                //TODO: add a reasonable exception wrapper here
                throw Exception(e);
            }
        }
        frame_buffer.setCapacity(0);

    }
}


WebSocket& WebSocketServerConnection::getSocket()
{
    return webSocket;
}

}
