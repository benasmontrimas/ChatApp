#pragma once

#include <cstdint>
#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <print>

#pragma comment(lib, "Ws2_32.lib")

#define MAX_CHANNEL_USER_COUNT    1'000
#define MAX_CHANNEL_MESSAGE_COUNT 100
#define MAX_CHANNEL_COUNT         10
#define MAX_USER_CHANNELS         100

constexpr const char* server_port           = "30302";
constexpr int         message_buffer_length = 512;
constexpr int         global_chat_id        = 0;
constexpr int         max_clients           = 100;

using u32 = uint32_t;
using u64 = uint64_t;

using UserID    = u32;
using ChannelID = u32;
using TimeStamp = u64;

#include "Message.h"

enum class ReturnCode {
        Success,
        FailedToConnectToSocket,
        FailedCreatingSocket,

        SendMessageFailed,

        ErrorUnknown,
};

// Server stores all messages and information on RAM, can move this to a database and then have it be persistent between runs, and also allows more
// messages to be stored.
struct Channel {
        std::string name;

        u32    user_count{};
        UserID users[MAX_CHANNEL_USER_COUNT];

        u32     message_count{};
        Message messages[MAX_CHANNEL_MESSAGE_COUNT];
};

struct User {
        UserID      id;
        std::string user_name;
        SOCKET      socket;
        u32         channel_count;
        ChannelID   channels[MAX_USER_CHANNELS];
};