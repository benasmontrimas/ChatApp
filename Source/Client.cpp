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

                if (message.sender == 0) {
                        // ===== Proccess Message from Server ======
                        ProcessServerMessage(message);
                } else {
                        // ===== Proccess Message from Users ======
                        Channel& channel                        = channels[message.channel];
                        channel.messages[channel.message_count] = message;
                        channel.message_count++;
                }
        }
}

void Client::ProcessServerMessage(const Message& message) {
        assert(message.sender == 0);

        ServerMessageType message_type{};
        memcpy(&message_type, &message.content[0], sizeof(ServerMessageType));

        switch (message_type) {
        case MessageUserListSync: {
                // ===== Update All Users In All Channels =====
                Channel& channel = channels[message.channel];

                std::println("Channel: {}", message.channel);

                u32 user_count = (message.content_length - sizeof(ServerMessageType)) / sizeof(UserID);

                for (u32 i = 0; i < user_count; i++) {
                        u32 index_into_content = i * sizeof(UserID) + sizeof(ServerMessageType);

                        UserID user_id;
                        memcpy(&user_id, &message.content[index_into_content], sizeof(UserID));

                        bool exists = false;
                        for (u32 user_idx = 0; user_idx < channel.user_count; user_idx++) {
                                UserID existing_user_id = channel.users[user_idx];
                                if (user_id == existing_user_id) {
                                        exists = true;
                                }
                        }

                        if (exists) continue;
                        message.content[sizeof(ServerMessageType) + sizeof(UserID)];
                        channel.users[channel.user_count] = user_id;
                        channel.user_count++;
                }

        } break;
        case MessageUserJoin: {
                // ===== Add User to Channel =====
                Channel& channel = channels[message.channel];

                UserID new_user;
                memcpy(&new_user, &message.content[sizeof(ServerMessageType)], sizeof(UserID));

                // TODO: Do we need to check if the user already exists?
                channel.users[channel.user_count] = new_user;
                channel.user_count++;
        } break;
        case MessageUserLeave: {
                // ===== Remove User to Channel =====
                Channel& channel = channels[message.channel];

                UserID leaving_user;
                memcpy(&leaving_user, &message.content[sizeof(ServerMessageType)], sizeof(UserID));

                for (u32 user_idx = 0; user_idx < channel.user_count; user_idx++) {
                        if (channel.users[user_idx] != leaving_user) continue;

                        channel.user_count--;
                        channel.users[user_idx] = channel.users[channel.user_count];
                        break;
                }
        } break;
        case MessageUserNameSend: {
                // ===== UserID User Name =====
                UserID user_id;
                memcpy(&user_id, &message.content[sizeof(ServerMessageType)], sizeof(UserID));

                u32 user_name_length = message.content_length - (sizeof(ServerMessageType) + sizeof(UserID));
                users[user_id].user_name.assign(&message.content[sizeof(ServerMessageType) + sizeof(UserID)], user_name_length);

                std::println("Trying to read Username: {}, With length: {}", users[user_id].user_name, user_name_length);
        } break;
        }
}

void Client::AddChannel(ChannelID id, const std::string& channel_name) {
        chat_channels[channel_count] = id;
        channel_count++;

        channels[id].name = channel_name;
}