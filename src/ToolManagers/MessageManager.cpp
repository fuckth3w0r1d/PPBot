#include "ToolManagers/MessageManager.h"
#include "logger.h"
#include "config.h"
#include <ctime>
#include <stdexcept>

namespace {
std::string messageId(const json& value)
{
    if(value.is_string()) return value.get<std::string>();
    if(value.is_number_integer()) return value.dump();
    return {};
}
}

ParsedMsgSegments MessageManager::parseMsgSegments(const json& msgsegs)
{
    ParsedMsgSegments result;
    for(const auto& seg : msgsegs)
    {
        if(!seg.is_object() || !seg.contains("type") || !seg["type"].is_string() ||
           !seg.contains("data") || !seg["data"].is_object()) continue;
        const auto type = seg["type"].get<std::string>();
        const auto& data = seg["data"];
        if(type == "at" && data.contains("qq") && messageId(data["qq"]) == BOT_QQ)
            result.at_me = true;
        if(type == "text" && data.contains("text") && data["text"].is_string())
        {
            result.has_text = true;
            result.text += data["text"].get<std::string>();
        }
        if(type == "image")
        {
            result.has_image = true;
            if(data.contains("url") && data["url"].is_string())
                result.image = data["url"].get<std::string>();
        }
        if(type == "json" && data.contains("data"))
        {
            result.has_json = true;
            if(data["data"].is_string())
                result.json_data = json::parse(data["data"].get<std::string>(), nullptr, false);
            else if(data["data"].is_object())
                result.json_data = data["data"];
        }
    }
    result.text.erase(0, result.text.find_first_not_of(' '));
    return result;
}

std::vector<SentMessage> MessageManager::send_msg(const MessageContext& recv, const json& reply, const std::string& sendType)
{
    httplib::Client cli(SERVER_HOST, SERVER_PORT);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);
    cli.set_write_timeout(30);
    httplib::Headers headers = {{"Authorization", "Bearer " + SERVER_TOKEN}};
    std::vector<SentMessage> sent;
    auto send = [&](const json& segments, bool forward) {
        json body;
        body[recv.msg_type == "group" ? "group_id" : "user_id"] =
            recv.msg_type == "group" ? recv.group_id : recv.user_id;
        std::string path;
        if(forward)
        {
            path = "/send_forward_msg";
            body["messages"] = json::array();
            for(const auto& seg : segments)
                body["messages"].push_back({{"type", "node"}, {"data", {
                    {"user_id", recv.user_id}, {"nickname", "Bot"},
                    {"content", json::array({seg})}
                }}});
        }
        else
        {
            path = recv.msg_type == "group" ? "/send_group_msg" : "/send_private_msg";
            body["message"] = segments;
        }
        const auto operation = "NapCat 消息发送 " +
            (recv.msg_type == "group" ? "group=" + recv.group_id : "user=" + recv.user_id) + " path=" + path;
        auto response = cli.Post(path, headers, body.dump(), "application/json");
        if(!response)
        {
            Logger::warn(operation + " 网络失败 reason=", httplib::to_string(response.error()));
            return;
        }
        if(response->status != 200)
        {
            Logger::httpError(operation, response->status, response->body);
            return;
        }
        const auto result = json::parse(response->body, nullptr, false);
        if(!result.is_object() || !result.contains("retcode") || !result["retcode"].is_number_integer() ||
           !result.contains("status") || !result["status"].is_string())
        {
            Logger::httpError(operation, response->status, response->body,
                result.is_discarded() ? "响应不是合法 JSON" : "响应缺少 retcode/status 有效字段");
            return;
        }
        if(result["retcode"] != 0 || result["status"] != "ok")
        {
            Logger::httpError(operation, response->status, response->body, "NapCat 未确认发送成功");
            return;
        }
        std::string id;
        if(result.contains("data") && result["data"].is_object() && result["data"].contains("message_id"))
            id = messageId(result["data"]["message_id"]);
        sent.push_back({segments, id});
        Logger::info("NapCat 发送成功 " + (recv.msg_type == "group" ? "group=" + recv.group_id : "user=" + recv.user_id) +
            " message_id=" + id + " segments=", segments.size());
    };
    json normal = json::array();
    json videos = json::array();
    for(const auto& seg : reply)
    {
        if(seg.is_object() && seg.value("type", "") == "video") videos.push_back(seg);
        else normal.push_back(seg);
    }
    if(!normal.empty()) send(normal, sendType != "direct");
    for(const auto& video : videos) send(json::array({video}), sendType != "direct");
    return sent;
}

MessageContext MessageManager::getMessageContext(const json& data)
{
    MessageContext msgctx;
    msgctx.msg_type = data.at("message_type").get<std::string>();
    msgctx.user_id = messageId(data.at("user_id"));
    if(msgctx.msg_type == "group") msgctx.group_id = messageId(data.at("group_id"));
    msgctx.msg_segments = data.at("message");
    if(msgctx.user_id.empty() || (msgctx.msg_type == "group" && msgctx.group_id.empty()) ||
       !msgctx.msg_segments.is_array()) throw std::invalid_argument("消息字段格式错误");
    if(data.contains("message_id")) msgctx.message_id = messageId(data["message_id"]);
    msgctx.timestamp = data.value("time", static_cast<int64_t>(std::time(nullptr)));
    if(data.contains("sender") && data["sender"].is_object())
    {
        msgctx.nickname = data["sender"].value("nickname", "");
        msgctx.card = data["sender"].value("card", "");
    }
    for(const auto& seg : msgctx.msg_segments)
    {
        if(seg.is_object() && seg.value("type", "") == "reply" &&
           seg.contains("data") && seg["data"].is_object() && seg["data"].contains("id"))
            msgctx.reply_to = messageId(seg["data"]["id"]);
    }
    msgctx.pmsgsegs = parseMsgSegments(msgctx.msg_segments);
    return msgctx;
}

json MessageManager::buildMsg(const std::string& msg_type, const std::string& msg_data)
{
    if(msg_type == "at") return {{"type", msg_type}, {"data", {{"qq", msg_data}}}};
    if(msg_type == "text") return {{"type", msg_type}, {"data", {{"text", msg_data}}}};
    if(msg_type == "image" || msg_type == "video" || msg_type == "record")
        return {{"type", msg_type}, {"data", {{"file", msg_data}}}};
    return {};
}
