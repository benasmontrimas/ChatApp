#pragma once

#include "ChatApp.h"

enum ReservedChannelIDs : ChannelID {
        ChannelIDServer = 0, // Messages for the server, not to be sent to any chat.
        ChannelIDGlobal = 1, // Chat which everyone who is connected to the server can see.

        ChannelIDUser = 100, // Start user IDs at 100. Leaves ChatIDs 0 - 99, for other internal uses.
};

struct Message {
        UserID    sender;                         // Set by server
        ChannelID channel;                        // Set by client
        TimeStamp timestamp;                      // Set by client
        u32       content_length;                 // Set by client
        char      content[message_buffer_length]; // Set by client
};

constexpr u32 message_size_in_bytes = sizeof(Message);