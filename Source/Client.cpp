#include "Client.h"
#include "Base.h"
#include "ChatApp.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <print>

ReturnCode Client::Init() {
        int res;

        res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        assert(res == 0 && "Failed Win Sock Startup");

        addrinfo* result{};
        addrinfo* ptr{};
        addrinfo  hints{};

        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags    = AI_PASSIVE;

        // const char* server_address = "2.0.118.94";
        const char* server_address = "";

        res = getaddrinfo(server_address, server_port, &hints, &result);
        if (res != 0) {
                std::println("Failed getaddrinfo function");
                WSACleanup();
                return ReturnCode::ErrorUnknown;
        }

        ptr           = result;
        // Windows example sets ptr to result then passes ptr here?
        client_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (client_socket == INVALID_SOCKET) {
                std::println("Failed creating socket");
                freeaddrinfo(result);
                WSACleanup();
                return ReturnCode::ErrorUnknown;
        }

        char addr_buffer[32];
        inet_ntop(result->ai_family, ptr->ai_addr, addr_buffer, 32);
        std::println("Connecting Client To Address: {}", addr_buffer);

        do {
                res = connect(client_socket, ptr->ai_addr, (int)ptr->ai_addrlen);
                ptr = ptr->ai_next;
        } while (res == SOCKET_ERROR and ptr != nullptr);

        freeaddrinfo(result);

        if (res == SOCKET_ERROR) {
                std::println("Error at socket(): {}", WSAGetLastError());
                std::println("Failed Connecting Socket");
                closesocket(client_socket);
                WSACleanup();
                return ReturnCode::FailedToConnectToSocket;
        }

        return ReturnCode::Success;
}

void Client::Shutdown() {
        shutdown(client_socket, SD_SEND);
        closesocket(client_socket);
        WSACleanup();
}

// Tells the server what to call this client.
ReturnCode Client::SendUserName(const std::string& user_name) {
        return SendMessage(ChannelIDServer, "username=" + user_name);
}

ReturnCode Client::SendMessage(const UserID channel, const std::string& message_string) {
        int res;

        // ===== Get Timestamp =====
        // NOTE: We get utc time, and use this to send as the time stamp, its up to the recieving client to convert this to sys time.
        std::chrono::time_point<std::chrono::utc_clock> time     = std::chrono::utc_clock::now();
        // NOTE: Timestamp is the seconds since
        std::chrono::duration<u64>                      duration = std::chrono::duration_cast<std::chrono::duration<u64>>(time.time_since_epoch());

        // ===== Create Message =====
        Message message{};
        message.channel        = channel;
        message.timestamp      = duration.count();
        // If string is too long, will just cut off the end.
        message.content_length = (u32)min((u32)message_string.length(), (u32)message_buffer_length);
        message_string.copy(message.content, message.content_length);

        // ===== Send Message =====
        int send_flags = 0;
        res            = send(client_socket, (char*)&message, sizeof(Message), send_flags);

        // ===== Process Error =====
        // TODO: Process failed message send, need to check server connection.
        if (res == SOCKET_ERROR) {
                std::println("Failed sending message");
                return ReturnCode::SendMessageFailed;
        }

        return ReturnCode::Success;
}

void Client::ProcessMessages() {
        int     res;
        Message message;

        while (true) {
                fd_set sockets_to_check{};
                sockets_to_check.fd_count    = 1;
                sockets_to_check.fd_array[0] = client_socket;

                timeval time_out_duration{ 0, 1 };
                int     num_sockets_ready = select(0, &sockets_to_check, nullptr, nullptr, &time_out_duration);
                if (num_sockets_ready == 0) break; // If no messages we just return

                int recieve_flags = 0;
                res               = recv(client_socket, (char*)&message, sizeof(message), recieve_flags);

                std::println("Recieved Message");

                // message.sender, channel, timestamp, content_length, content,

                channel_messages[message.channel].push_back(message);
        }
}

void Client::AddChannel(ChannelID id, const std::string& channel_name) {
        chat_channels[channel_count]      = id;
        chat_channel_names[channel_count] = channel_name;
        channel_count++;
}