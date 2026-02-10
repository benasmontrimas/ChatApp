#pragma once

#include <cstdint>
#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <print>

#pragma comment(lib, "Ws2_32.lib")

constexpr const char* server_port           = "30302";
constexpr int         message_buffer_length = 512;
constexpr int         global_chat_id        = 0;
constexpr int         max_clients           = 100;

using u32 = uint32_t;
using u64 = uint64_t;

using UserID    = u32;
using ChannelID = u32;
using TimeStamp = u64;

enum class ReturnCode {
        Success,
        FailedToConnectToSocket,
        FailedCreatingSocket,

        SendMessageFailed,

        ErrorUnknown,
};