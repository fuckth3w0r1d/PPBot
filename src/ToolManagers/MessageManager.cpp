#include "ToolManagers/MessageManager.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

////////////
// 消息管理器
///////////

// 消息段解析
ParsedMsgSegments MessageManager::parseMsgSegments(const json& msgsegs)
{
    ParsedMsgSegments result;
    for(auto& seg : msgsegs)
    {
        if(!seg.contains("type") || !seg.contains("data")) continue;
        std::string type = seg["type"].get<std::string>();
        if(type == "at")
        {
            if(seg["data"].contains("qq") && seg["data"]["qq"] == BOT_QQ) 
                result.at_me = true;
        }
        if(type == "text")
        {
            if (seg["data"].contains("text"))
            {
                result.has_text = true;
                result.text += seg["data"]["text"].get<std::string>();
            }
        }
        if(type == "image")
        {
            result.has_image = true;
            if(seg["data"].contains("url"))
            {
                result.image = seg["data"]["url"].get<std::string>();
            }
        }
        if(type == "json")
        {
            result.has_json = true;
            if(seg["data"].contains("data"))
            {
                result.json_data = json::parse(seg["data"]["data"].get<std::string>());
            }
        }
    }
    while(!result.text.empty() && result.text[0] == ' ')
    {   // 去除文本消息的前置空格
        result.text.erase(0, 1);
    }
    return result;
}

// 统一发送消息接口
void MessageManager::send_msg(const MessageContext& recv, const json& reply)
{
    httplib::Client cli(SERVER_HOST, SERVER_PORT);
    json normal_segments = json::array();
    json video_segments = json::array();
    // 分类 segment
    for(const auto& seg : reply)
    {
        if(seg.contains("type") && seg["type"] == "video")
            video_segments.push_back(seg);
        else
            normal_segments.push_back(seg);
    }
    httplib::Headers headers = {
        {"Authorization", "Bearer " + SERVER_TOKEN}
    };
    std::string path = (recv.msg_type == "group") ? "/send_group_msg" : "/send_private_msg";
    // 发送普通消息
    if(!normal_segments.empty())
    {
        json body;
        if(recv.msg_type == "group")
            body["group_id"] = recv.group_id;
        else
            body["user_id"] = recv.user_id;

        body["message"] = normal_segments;
        auto res = cli.Post(path, headers, body.dump(), "application/json");
        if(!res)
        {
            Logger::error("消息发送失败", httplib::to_string(res.error()));
        }
        else if(res->status != 200)
        {
            Logger::warn("send_msg HTTP状态码: ", res->status);
            Logger::error("send_msg 异常响应体:", json::parse(res->body).dump(4));
        }
    }
    // 单独发送 video
    for(const auto& seg : video_segments)
    {
        json body;
        if(recv.msg_type == "group")
            body["group_id"] = recv.group_id;
        else
            body["user_id"] = recv.user_id;
        body["message"] = json::array({seg});
        auto res = cli.Post(path, headers, body.dump(), "application/json");
        if(!res)
        {
            Logger::error("视频发送失败", httplib::to_string(res.error()));
        }
        else if(res->status != 200)
        {
            Logger::warn("video HTTP状态码: ", res->status);
            Logger::error("video 异常响应体:", json::parse(res->body).dump(4));
        }
    }
}
// 获取消息结构
MessageContext MessageManager::getMessageContext(const json& data)
{
    MessageContext msgctx;
    msgctx.msg_type = data["message_type"].get<std::string>();
    if(msgctx.msg_type == "group") msgctx.group_id = std::to_string(data["group_id"].get<size_t>());
    msgctx.user_id = std::to_string(data["user_id"].get<size_t>());
    msgctx.msg_segments = data["message"].get<json>();
    msgctx.pmsgsegs = parseMsgSegments(msgctx.msg_segments);
    return msgctx;
}
// 构造array格式消息
json MessageManager::buildMsg(const std::string& msg_type, const std::string& msg_data)
{
    json result;
    if(msg_type == "at")
    {
        json data;
        data["qq"] = msg_data;
        result["data"] = data;
        result["type"] = msg_type;
    }
    if(msg_type == "text")
    {
        json data;
        data["text"] = msg_data;
        result["data"] = data;
        result["type"] = msg_type;
    }
    if(msg_type == "image" || msg_type == "video")
    {
        json data;
        data["file"] = msg_data;
        result["data"] = data;
        result["type"] = msg_type;
    }
    return result;
}
