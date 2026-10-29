#pragma once

#include <string>
#include <vector>
#include <cstdint>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

// 消息结构
struct ParsedMsgSegments{
    bool at_me = false;
    bool has_text = false;
    bool has_image = false;
    bool has_json = false;

    std::string text;        // 文本
    std::string image;       // image url
    json json_data;          // json
};

struct MessageContext{
    std::string group_id;
    std::string user_id;
    std::string msg_type;
    json msg_segments;
    ParsedMsgSegments pmsgsegs;
    std::string message_id;
    std::string nickname;
    std::string card;
    std::string reply_to;
    int64_t timestamp = 0;
    uint64_t timeline_sequence = 0;
    bool reply_to_bot = false;
    bool is_poke = false;
    std::string target_id;
};

struct SentMessage{
    json segments;
    std::string message_id;
    bool is_poke = false;
};

// 消息管理器
class MessageManager{
private:
    static ParsedMsgSegments parseMsgSegments(const json& msgsegs);

public:
    static std::vector<SentMessage> send_msg(const MessageContext& recv, const json& reply, const std::string& sendType);
    static MessageContext getMessageContext(const json& data);
    static bool isGroupPoke(const json& data);
    static json buildMsg(const std::string& msg_type, const std::string& msg_data);
};
