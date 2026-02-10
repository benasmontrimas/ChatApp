#include "Server.h"

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
        channel_count = 1;
        channels[0]   = {};
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

void ProcessMessage(Server* server, SOCKET client_socket, const Message& message) {
        switch (message.channel) {
        case ChannelIDServer: {
                // ===== Handle Server Message =====
                // TODO: Get option, and handle
                // NOTE: If username change we will need to send this through to all clients so that they can update their local name for that user.
                std::string content(message.content, message.content_length);
                if (content.starts_with("username")) {
                        std::println("Set user {} username to {}", message.sender, content.substr(9));
                }
        } break;
        case ChannelIDGlobal: {
                // ===== Handle Global Message =====
                // TODO: Set message sender as client, and send to all connected clients.

                std::println("Message from client recieved: {}", message.content);

                for (u32 i = 0; i < server->channels[0].user_count; i++) {
                        // NOTE: This just sends back to who sent it, we dont actually know other users sockets, need to store.
                        int send_flags = 0;
                        send(server->user_sockets[server->channels[0].users[i]], (char*)&message, sizeof(Message), send_flags);
                }

        } break;

        default:
                // ===== Handle Group Chat Message =====
                // TODO: Set sender and send to recipient.
                break;
        }
}

// NOTE: Send message to all client to tell them the server is down.
void ProcessClient(Server* server, SOCKET client_socket, bool& running) {
        int res;

        Message message;

        while (running) {
                // NOTE: Call select first to check status of the socket, it allows specifying a time out so we can check if the server is still
                // running whilst waiting for message.
                fd_set sockets_to_check{};
                sockets_to_check.fd_count    = 1;
                sockets_to_check.fd_array[0] = client_socket;

                timeval time_out_duration{ 0, 100 };
                int     num_sockets_ready = select(0, &sockets_to_check, nullptr, nullptr, &time_out_duration); // 100 ms timeout
                if (num_sockets_ready == 0) continue;

                int recieve_flags = 0;
                res               = recv(client_socket, (char*)&message, sizeof(message), recieve_flags);

                if (res > 0) { // Success
                        ProcessMessage(server, client_socket, message);
                } else if (res == 0) { // Closing Connection
                        std::println("res == 0");
                        break;
                } else { // Error
                        std::println("Recieve failed");
                        closesocket(client_socket);
                        return;
                }
        }

        closesocket(client_socket);
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
        UserID next_chat_id = 0;

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

                // ===== Add to Global Channel =====
                channels[0].users[channels[0].user_count] = client_id;
                channels[0].user_count++;

                // ===== Add User Socket To Map =====
                user_sockets[client_id] = client_socket;

                // ===== Assign Client to thread =====
                client_threads[client_count] = std::thread(&ProcessClient, this, client_socket, std::ref(running));
                client_count++;
        }
}