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

        return Reconnect();
}

void Client::Shutdown() {
        shutdown(client_socket, SD_SEND);
        closesocket(client_socket);
        WSACleanup();
}

ReturnCode Client::Reconnect() {
        int res;

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
                return ReturnCode::FailedToConnectToSocket;
        }

        return ReturnCode::Success;
}

// Tells the server what to call this client.
ReturnCode Client::SendUserName(const std::string& user_name) {
        return SendMessage(ChannelIDServer, "username=" + user_name);
}

ReturnCode Client::SendMessage(ChannelID channel, const std::string& message_string) {
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

ReturnCode Client::Ping() {
        Message message{};
        message.sender  = id;
        message.channel = ChannelIDServer;

        ServerMessageType message_type = MessagePing;
        memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
        message.content_length += sizeof(ServerMessageType);

        // ===== Send Message =====
        int send_flags = 0;
        int res        = send(client_socket, (char*)&message, sizeof(Message), send_flags);

        if (res == SOCKET_ERROR) {
                std::println("Failed sending message");
                return ReturnCode::SendMessageFailed;
        }

        return ReturnCode::Success;
}

void Client::CreatePrivateMessageChannel(UserID user_id) {
        Message message{};

        message.channel   = ChannelIDServer;
        message.timestamp = 0;

        // ===== Write Message Type =====
        ServerMessageType message_type = MessageCreateChannel;
        memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
        message.content_length += sizeof(ServerMessageType);

        // ===== Write Invited User ID =====
        memcpy(&message.content[message.content_length], &user_id, sizeof(UserID));
        message.content_length += sizeof(UserID);

        // ===== Send Message =====
        int send_flags = 0;
        send(client_socket, (char*)&message, sizeof(Message), send_flags);
}

void Client::InviteUserToChannel(UserID user_id, ChannelID channel_id) {
        Message message{};

        message.channel   = ChannelIDServer;
        message.timestamp = 0;

        // ===== Write Message Type =====
        ServerMessageType message_type = MessageUserInvite;
        memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
        message.content_length += sizeof(ServerMessageType);

        // ===== Write Invited Channel ID =====
        memcpy(&message.content[message.content_length], &channel_id, sizeof(ChannelID));
        message.content_length += sizeof(ChannelID);

        // ===== Write Invited User ID =====
        memcpy(&message.content[message.content_length], &user_id, sizeof(UserID));
        message.content_length += sizeof(UserID);

        // ===== Send Message =====
        int send_flags = 0;
        send(client_socket, (char*)&message, sizeof(Message), send_flags);
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

                if (res == INVALID_SOCKET) return;

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
        case MessageUserIDGet: {
                memcpy(&id, &message.content[sizeof(ServerMessageType)], sizeof(UserID));
        } break;
        case MessageUserListSync: {
                // ===== Update All Users In All Channels =====
                Channel& channel = channels[message.channel];

                std::println("Channel: {}", message.channel);

                u32 user_count = (message.content_length - sizeof(ServerMessageType)) / sizeof(UserID);

                for (u32 i = 0; i < user_count; i++) {
                        u32 index_into_content = i * sizeof(UserID) + sizeof(ServerMessageType);

                        UserID user_id;
                        memcpy(&user_id, &message.content[index_into_content], sizeof(UserID));

                        std::println("Sync Channel: {}, with user: {}", message.channel, user_id);

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

                u32         user_name_length = message.content_length - (sizeof(ServerMessageType) + sizeof(UserID));
                std::string new_user_name(&message.content[sizeof(ServerMessageType) + sizeof(UserID)], user_name_length);

                bool user_exists = false;
                for (u32 user_idx = 0; user_idx < channel.user_count; user_idx++) {
                        UserID user_id = channel.users[user_idx];
                        if (user_id == new_user) {
                                user_exists = true;
                                break;
                        }
                }

                if (!user_exists) {
                        channel.users[channel.user_count] = new_user;
                        channel.user_count++;
                }

                // ===== Store Message To Display Join =====
                Message     display_message = message;
                const char* joined_text     = " Joined";
                new_user_name.copy(display_message.content, user_name_length);
                memcpy(&display_message.content[user_name_length], joined_text, 8);
                display_message.content_length = user_name_length + 8;

                channel.messages[channel.message_count] = display_message;
                channel.message_count++;
        } break;
        case MessageUserLeave: {
                UserID leaving_user;
                memcpy(&leaving_user, &message.content[sizeof(ServerMessageType)], sizeof(UserID));

                User& user = users[leaving_user];

                u32         user_name_length = message.content_length - (sizeof(ServerMessageType) + sizeof(UserID));
                std::string leaving_user_name(&message.content[sizeof(ServerMessageType) + sizeof(UserID)], user_name_length);

                // ===== Remove User from All Channel =====
                for (u32 channel_idx = 0; channel_idx < user.channel_count; channel_idx++) {
                        ChannelID channel_id = user.channels[channel_idx];
                        Channel&  channel    = channels[channel_id];

                        for (u32 user_idx = 0; user_idx < channel.user_count; user_idx++) {
                                if (channel.users[user_idx] != leaving_user) continue;

                                channel.user_count--;
                                channel.users[user_idx] = channel.users[channel.user_count];
                                break;
                        }


                        // ===== Store Message To Display Leave =====
                        Message     display_message = message;
                        const char* left_text       = " Left";
                        leaving_user_name.copy(display_message.content, user_name_length);
                        memcpy(&display_message.content[user_name_length], left_text, 6);
                        display_message.content_length = user_name_length + 6;

                        channel.messages[channel.message_count] = display_message;
                        channel.message_count++;
                }
        } break;
        case MessageUserLeaveChannel: {
                // ===== Remove User from Channel =====
                Channel& channel = channels[message.channel];

                UserID leaving_user;
                memcpy(&leaving_user, &message.content[sizeof(ServerMessageType)], sizeof(UserID));

                u32         user_name_length = message.content_length - (sizeof(ServerMessageType) + sizeof(UserID));
                std::string leaving_user_name(&message.content[sizeof(ServerMessageType) + sizeof(UserID)], user_name_length);

                for (u32 user_idx = 0; user_idx < channel.user_count; user_idx++) {
                        if (channel.users[user_idx] != leaving_user) continue;

                        channel.user_count--;
                        channel.users[user_idx] = channel.users[channel.user_count];
                        break;
                }

                // ===== Store Message To Display Leave =====
                Message     display_message = message;
                const char* joined_text     = " Left";
                leaving_user_name.copy(display_message.content, user_name_length);
                memcpy(&display_message.content[user_name_length], joined_text, 6);
                display_message.content_length = user_name_length + 6;

                channel.messages[channel.message_count] = display_message;
                channel.message_count++;

                // ===== If This is The Leaver Remove =====
                if (leaving_user == id) {
                        channels.erase(message.channel);
                        for (u32 channel_idx = 0; channel_idx < channel_count; channel_idx++) {
                                if (chat_channels[channel_idx] == message.channel) {
                                        channel_count--;
                                        chat_channels[channel_idx] = chat_channels[channel_count];
                                }
                        }
                }
        }
        case MessageUserNameSend: {
                // ===== UserID User Name =====
                UserID user_id;
                memcpy(&user_id, &message.content[sizeof(ServerMessageType)], sizeof(UserID));

                u32 user_name_length = message.content_length - (sizeof(ServerMessageType) + sizeof(UserID));
                users[user_id].id    = user_id;
                users[user_id].user_name.assign(&message.content[sizeof(ServerMessageType) + sizeof(UserID)], user_name_length);
        } break;
        case MessageUserNewChannel: {
                // ===== Channel ID =====
                ChannelID channel_id;
                memcpy(&channel_id, &message.content[sizeof(ServerMessageType)], sizeof(ChannelID));

                std::println("Added To Channel: {}", channel_id);

                // ===== Check if Channel Exists =====
                bool exists = false;
                for (u32 channel_idx = 0; channel_idx < channel_count; channel_idx++) {
                        ChannelID current_channel_id = chat_channels[channel_idx];
                        if (current_channel_id != channel_id) continue;
                        exists = true;
                        break;
                }

                // ===== Add Channel If Doesnt Exist =====
                if (!exists) {
                        chat_channels[channel_count] = channel_id;
                        channel_count++;
                        channels[channel_id] = {};
                }

                // ===== Channel Name ======
                u32 channel_name_length = message.content_length - (sizeof(ServerMessageType) + sizeof(ChannelID));
                channels[channel_id].name.assign(&message.content[sizeof(ServerMessageType) + sizeof(ChannelID)], channel_name_length);
        } break;
        }
}

// NOTE: Once we recieve the message that we leave is when we actually leave.
void Client::LeaveChannel(ChannelID id) {
        Channel& channel = channels[id];

        Message message{};
        message.sender    = id;
        message.channel   = ChannelIDServer;
        message.timestamp = 0;

        // ===== Write Message Type =====
        ServerMessageType message_type = MessageUserLeaveChannel;
        memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
        message.content_length += sizeof(ServerMessageType);

        // ===== Write Channel ID =====
        memcpy(&message.content[message.content_length], &id, sizeof(ChannelID));
        message.content_length += sizeof(ChannelID);

        // ===== Send Message =====
        int send_flags = 0;
        send(client_socket, (char*)&message, sizeof(Message), send_flags);
}

void Client::AddChannel(ChannelID id, const std::string& channel_name) {
        chat_channels[channel_count] = id;
        channel_count++;

        channels[id].name = channel_name;
}