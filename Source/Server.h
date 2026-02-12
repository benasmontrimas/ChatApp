#pragma once

#include "ChatApp.h"
#include "Message.h"

#include <thread>
#include <unordered_map>

/*
SERVER SETTINGS (adjustable from GUI):
        - Local/Remote
        - Max Clients
        - Max Message Size
        - Port
        - GUI enabled (can have a start up gui, which closes if no gui enabled leaving just a console, or if gui is enabled loads gui showing clients and
messages);
*/


/*
NOTES:
- Server Messages:
        messages sent to ChatIDServer will get processed as commands to the server. the contents will be read as option=value
        options:
        - username, sets the clients username.
        - new_channel, sets up a new channel, the server must send a message back to specify the channel id. NOTE: we can store
        admins, and the user that created the channel defaults to an admin and can set other users as admins.
*/

struct Server {
        // NOTE: Add flag to allow only a local server.
        void Init();
        void Shutdown();

        void Run();

        void AddUserToChannel(ChannelID channel_id, UserID new_user_id);

        WSADATA wsa_data;
        SOCKET  listener_socket{ INVALID_SOCKET };

        int         client_count{};
        std::thread client_threads[max_clients];

        std::unordered_map<UserID, User>       users;
        std::unordered_map<ChannelID, Channel> channels;

        bool running;
};