#pragma once

#include "ChatApp.h"

enum ServerMessageType : u32 {
        MessageNone,

        MessagePing,

        MessageUserIDGet,
        MessageUserListSync,
        MessageUserJoin,
        MessageUserLeave,
        MessageUserLeaveChannel,
        MessageUserNameRequest,
        MessageUserNameSend,

        MessageUserNewChannel,
        MessageCreateChannel,
        MessageUserInvite, // Not really an invite, as your forced into the channel.

};

struct Message {
        UserID    sender;                         // Set by server
        ChannelID channel;                        // Set by client
        TimeStamp timestamp;                      // Set by client
        u32       content_length;                 // Set by client
        char      content[message_buffer_length]; // Set by client
};

constexpr u32 message_size_in_bytes = sizeof(Message);