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

        ReturnCode Reconnect();

        // ===== Functions to send messages to the server =====
        ReturnCode SendUserName(const std::string& user_name);
        ReturnCode SendMessage(ChannelID channel, const std::string& message);
        ReturnCode Ping();
        void       CreatePrivateMessageChannel(UserID user_id);
        void       InviteUserToChannel(UserID user_id, ChannelID channel_id);

        // ===== Functions to process messages from the server =====
        void ProcessMessages();
        void ProcessServerMessage(const Message& message);

        // ===== Util functions =====
        void LeaveChannel(ChannelID id);
        void AddChannel(ChannelID id, const std::string& channel_name);

        // ===== Socket Data =====
        WSADATA wsa_data;
        SOCKET  client_socket{ INVALID_SOCKET };

        // ===== ID =====
        UserID id;

        // ===== Channels Info =====
        // NOTE: I dont remember why I wanted an array of chat channels as well as a map?
        u32                                    channel_count{};
        ChannelID                              chat_channels[MAX_CHAT_CHANNEL_COUNT]{};
        std::unordered_map<ChannelID, Channel> channels{};

        // ===== User Data =====
        std::unordered_map<UserID, User> users{};
};