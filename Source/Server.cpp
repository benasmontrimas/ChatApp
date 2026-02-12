#include "Server.h"

void SyncUsers(Server* server, User user);
void SendUserName(Server* server, User sender_user, UserID wanted_user_id);

void Server::Init() {
        int res;

        res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        std::println("{}", res);
        // assert(res == 0 && "Failed Win Sock Startup");

        addrinfo* result{};
        addrinfo* ptr{};
        addrinfo  hints{};

        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags    = AI_PASSIVE;

        res = getaddrinfo(NULL, server_port, &hints, &result);
        if (res != 0) {
                std::println("Failed getaddrinfo function");
                WSACleanup();
                return;
        }

        listener_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (listener_socket == INVALID_SOCKET) {
                std::println("Failed creating socket");
                freeaddrinfo(result);
                WSACleanup();
                return;
        }

        char addr_buffer[32];
        inet_ntop(result->ai_family, result->ai_addr, addr_buffer, 32);
        std::println("Binding Server To Address: {}", addr_buffer);

        res = bind(listener_socket, result->ai_addr, (int)result->ai_addrlen);

        freeaddrinfo(result);

        if (res == SOCKET_ERROR) {
                std::println("Failed Binding Socket");
                closesocket(listener_socket);
                WSACleanup();
                return;
        }

        res = listen(listener_socket, SOMAXCONN);
        if (res == SOCKET_ERROR) {
                std::println("Failed Listening");
                closesocket(listener_socket);
                WSACleanup();
                return;
        }

        running = true;

        // ===== Create Global Channel =====
        channels[ChannelIDGlobal] = {};
}

void Server::Shutdown() {
        running = false;

        // Need to ensure AcceptConnections is no longer running so that we dont accept more clients...

        for (int i = 0; i < client_count; i++) {
                client_threads[i].join();
        }

        closesocket(listener_socket);
        WSACleanup();
}

void ProcessMessage(Server* server, User user, Message& message) {
        switch (message.channel) {
        case ChannelIDServer: {
                // ===== Handle Server Message =====
                // TODO: Get option, and handle
                // NOTE: If username change we will need to send this through to all clients so that they can update their local name for that user.
                std::string content(message.content, message.content_length);
                if (content.starts_with("username")) {
                        std::println("Set user {} username to {}", user.id, content.substr(9));

                        server->users[user.id].user_name = std::string(content.substr(9));
                        return;
                }

                ServerMessageType message_type{};
                memcpy(&message_type, &message.content[0], sizeof(ServerMessageType));

                switch (message_type) {
                case MessageUserListSync: {
                        SyncUsers(server, user);
                } break;
                case MessageUserNameRequest: {
                        UserID wanted_user_id;
                        memcpy(&wanted_user_id, &message.content[sizeof(ServerMessageType)], sizeof(UserID));

                        SendUserName(server, user, wanted_user_id);
                } break;
                }
        } break;
        case ChannelIDGlobal: {
                // ===== Handle Global Message =====
                // TODO: Set message sender as client, and send to all connected clients.

                std::println("Message from client recieved: {}", message.content);

                message.sender = user.id;

                for (u32 i = 0; i < server->channels[ChannelIDGlobal].user_count; i++) {
                        int    send_flags = 0;
                        UserID user_id    = server->channels[ChannelIDGlobal].users[i];
                        send(server->users[user_id].socket, (char*)&message, sizeof(Message), send_flags);

                        std::println("Sending to: {}", user_id);
                        std::println("Sending from: {}", message.sender);
                }

        } break;

        default:
                // ===== Handle Group Chat Message =====
                // TODO: Set sender and send to recipient.
                break;
        }
}

// Send all users in all servers the user is a part of.
/*
Message format:
u32: Count -> We can also store this in content size
u32: MesageType = ConnectedUsers
u32: ChannelID
u32: UserID[]
*/
void SyncUsers(Server* server, User user) {
        Message message{};
        message.sender         = 0; // Not used - If we reserve 0 for server this can be useful
        message.timestamp      = 0;
        message.content_length = 0;

        // ====== Write Message Type to message =====
        ServerMessageType message_type = MessageUserListSync;
        memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
        message.content_length += sizeof(ServerMessageType);

        for (u32 channel_idx = 0; channel_idx < user.channel_count; channel_idx++) {
                ChannelID channel_id = user.channels[channel_idx];
                Channel&  channel    = server->channels[channel_id];

                // ===== Set Channel ID =====
                message.channel = channel_id;

                for (u32 user_idx = 0; user_idx < channel.user_count; user_idx++) {
                        UserID user_id = channel.users[user_idx];

                        // ===== Write User ID to message =====
                        memcpy(&message.content[message.content_length], &user_id, sizeof(user_id));
                        message.content_length += sizeof(user_id);

                        if (message.content_length + sizeof(user_id) > message_buffer_length) {
                                // ===== Send Message =====
                                int send_flags = 0;
                                send(user.socket, (char*)&message, sizeof(Message), send_flags);

                                // ===== Clear Content =====
                                message.content_length = sizeof(ServerMessageType);
                        }
                }

                // ===== If no users we dont need to send =====
                if (message.content_length <= sizeof(ServerMessageType)) continue;

                // ===== Send =====
                int send_flags = 0;
                send(user.socket, (char*)&message, sizeof(Message), send_flags);
        }
}

void SendUserName(Server* server, User sender_user, UserID wanted_user_id) {
        Message message{};
        message.sender    = 0;
        message.timestamp = 0;

        // ===== Write Message Type =====
        ServerMessageType message_type = MessageUserNameSend;
        memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
        message.content_length += sizeof(ServerMessageType);

        // ===== Write User ID =====
        memcpy(&message.content[sizeof(ServerMessageType)], &wanted_user_id, sizeof(UserID));
        message.content_length += sizeof(UserID);

        // ===== Write User Name =====
        User& wanted_user     = server->users[wanted_user_id];
        u32   username_length = (u32)wanted_user.user_name.size();
        memcpy(&message.content[message.content_length], wanted_user.user_name.c_str(), username_length);
        message.content_length += username_length;

        // ===== Send Message =====
        int send_flags = 0;
        send(sender_user.socket, (char*)&message, sizeof(Message), send_flags);
}

// NOTE: Send message to all client to tell them the server is down.
void ProcessClient(Server* server, User user, bool& running) {
        int res;

        SyncUsers(server, user);

        Message message;

        while (running) {
                // NOTE: Call select first to check status of the socket, it allows specifying a time out so we can check if the server is still
                // running whilst waiting for message.
                fd_set sockets_to_check{};
                sockets_to_check.fd_count    = 1;
                sockets_to_check.fd_array[0] = user.socket;

                timeval time_out_duration{ 0, 100 };
                int     num_sockets_ready = select(0, &sockets_to_check, nullptr, nullptr, &time_out_duration); // 100 ms timeout
                if (num_sockets_ready == 0) continue;

                int recieve_flags = 0;
                res               = recv(user.socket, (char*)&message, sizeof(message), recieve_flags);

                if (res > 0) { // Success
                        message.sender = user.id;
                        ProcessMessage(server, user, message);
                } else if (res == 0) { // Closing Connection
                        std::println("res == 0");
                        break;
                } else { // Error
                        std::println("Recieve failed");
                        closesocket(user.socket);
                        return;
                }
        }

        closesocket(user.socket);
        server->users.erase(user.id);
        // TODO: Remove user from all channels, and send message to all users that the user has left.
}

void Server::AddUserToChannel(ChannelID channel_id, UserID new_user_id) {
        // ===== Add The User to Users List =====
        channels[channel_id].users[channels[channel_id].user_count] = new_user_id;
        channels[channel_id].user_count++;

        users[new_user_id].channels[users[new_user_id].channel_count] = channel_id;
        users[new_user_id].channel_count++;

        // ===== Broadcast the new user to all existing users =====
        for (u32 user_idx = 0; user_idx < channels[channel_id].user_count; user_idx++) {
                UserID user_id = channels[channel_id].users[user_idx];
                if (user_id == new_user_id) continue; // dont send to ourselves.

                User& user = users[user_id];

                Message message{};
                message.sender         = 0;
                message.channel        = channel_id;
                message.timestamp      = 0;
                message.content_length = sizeof(ServerMessageType) + sizeof(UserID);

                ServerMessageType message_type = MessageUserJoin;
                memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
        }
}

// Want this to be runnable from a seperate thread, so that we can create a server GUI if we want.
// Or make it a single run function which the user needs to call in a loop. Means we have no race conditions for checking server data.
// However this would introduce latency between connects, but this would probably be not noticable compared to the latency of the network.
// (obvously this doesnt apply to local networks).
void Server::Run() {
        std::println("Waiting on Clients");

        // NOTE: Without accounts we have no way to actually identify users if they are on the same pc,
        // so we just have a rolling ID that resets every server run. When IDs are reused it doesnt matter,
        // as the messages arnt stored. If they were, we would want to have unique IDs per user, so that we
        // can still reference users.
        UserID next_chat_id = 1; // Reserve 0 for server messages.

        while (running) {
                fd_set sockets_to_check{};
                sockets_to_check.fd_count    = 1;
                sockets_to_check.fd_array[0] = listener_socket;
                timeval time_out_duration{ 0, 100 };
                int     num_sockets_ready = select(0, &sockets_to_check, nullptr, nullptr, &time_out_duration);
                if (num_sockets_ready == 0) continue;

                SOCKET client_socket = INVALID_SOCKET;

                client_socket = accept(listener_socket, NULL, NULL);
                if (client_socket == INVALID_SOCKET) {
                        continue;
                }

                std::println("Successfully Connected");

                // ===== Get User ID =====
                UserID client_id = next_chat_id;
                next_chat_id++;

                // ===== Add User Info =====
                users[client_id].id            = client_id;
                users[client_id].socket        = client_socket;
                users[client_id].channel_count = 0;

                // ===== Add to Global Channel =====
                AddUserToChannel(ChannelIDGlobal, client_id);

                // ===== Assign Client to thread =====
                client_threads[client_count] = std::thread(&ProcessClient, this, users[client_id], std::ref(running));
                client_count++;
        }
}