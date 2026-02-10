#pragma once

#include "ChatApp.h"
#include "Message.h"

#include <string>
#include <unordered_map>
#include <vector>

#define MAX_CHAT_CHANNEL_COUNT 1'000

struct Client {
        ReturnCode Init();
        void       Shutdown();

        // ===== Functions to send messages to the server =====
        ReturnCode SendUserName(const std::string& user_name);
        ReturnCode SendMessage(const UserID channel, const std::string& message);

        // ===== Functions to process messages from the server =====
        void ProcessMessages();

        // ===== Util functions =====
        void AddChannel(ChannelID id, const std::string& channel_name);

        // ===== Socket Data =====
        WSADATA wsa_data;
        SOCKET  client_socket{ INVALID_SOCKET };

        // ===== Channels Info =====
        u32         channel_count{};
        ChannelID   chat_channels[MAX_CHAT_CHANNEL_COUNT]{};
        std::string chat_channel_names[MAX_CHAT_CHANNEL_COUNT]{};

        // ===== Per Channel Info =====
        std::unordered_map<ChannelID, std::vector<UserID>>  channel_users{};
        std::unordered_map<ChannelID, std::vector<Message>> channel_messages{};
};