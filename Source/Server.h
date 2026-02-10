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

#define MAX_CHANNEL_USER_COUNT    1'000
#define MAX_CHANNEL_MESSAGE_COUNT 100
#define MAX_CHANNEL_COUNT         10

// Server stores all messages and information on RAM, can move this to a database and then have it be persistent between runs, and also allows more
// messages to be stored.
struct Channel {
        u32    user_count{};
        UserID users[MAX_CHANNEL_USER_COUNT];

        u32     message_count{};
        Message messages[MAX_CHANNEL_MESSAGE_COUNT];
};

struct Server {
        // NOTE: Add flag to allow only a local server.
        void Init();
        void Shutdown();

        void Run();

        WSADATA wsa_data;
        SOCKET  listener_socket{ INVALID_SOCKET };

        int         client_count{};
        std::thread client_threads[max_clients];

        std::unordered_map<UserID, SOCKET>      user_sockets;
        std::unordered_map<UserID, std::string> user_names;

        // Needs to either be map or have a seperate lookup into this array for ChannelID to index. (just use a map to store channels);
        u32     channel_count{};
        Channel channels[MAX_CHANNEL_COUNT];

        bool running;
};