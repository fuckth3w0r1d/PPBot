#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

std::string MusicCommand::getMusicId(const std::string& tag)
{
    httplib::Client cli(MUSIC_HOST, MUSIC_PORT); // 如果api支持改为SSLClient
    auto res = cli.Get(MUSIC_GET_ID_PATH + "?keywords=" + tag);
    if(!res)
    {
        Logger::error("获取歌曲 id 请求失败", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 200)
    {
        Logger::warn("获取歌曲 id  HTTP状态码: ", res->status);
        Logger::error("获取歌曲 id  异常响应体:", json::parse(res->body).dump(4));
        return "";
    }
    Logger::debug("开始获取id:", "解析body");
    json data = json::parse(res->body);
    Logger::debug("开始获取id:", "提取id");
    std::string id = std::to_string(data["result"]["songs"][0]["id"].get<size_t>());
    Logger::debug("获取id:", id);
    return id;
}

std::string MusicCommand::getMusicUrl(const std::string& id)
{
    httplib::Client cli(MUSIC_HOST, MUSIC_PORT); // 如果api支持改为SSLClient
    auto res = cli.Get(MUSIC_GET_URL_PATH + "?id=" + id + "&level=exhigh");
    if(!res)
    {
        Logger::error("随机图片网络请求失败", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 200)
    {
        Logger::warn("随机图片请求 HTTP状态码: ", res->status);
        Logger::error("随机图片请求 异常响应体:", json::parse(res->body).dump(4));
        return "";
    }
    Logger::debug("开始获取url:", "解析body");
    json data = json::parse(res->body);
    Logger::debug("开始获取url:", "提取url");
    std::string url = data["data"][0]["url"].get<std::string>();
    Logger::debug("获取url:", url);
    return url;
}

std::string MusicCommand::name() 
{
    return "点歌";
}

json MusicCommand::execute(const std::string& args)
{
    json result;
    if(args.empty())
    {
        return MessageManager::buildMsg("text", "请输入歌曲名称");
    }
    std::string id = getMusicId(args);
    if(id.empty())
    {
        result = MessageManager::buildMsg("text", "搜索取歌曲失败");
    }else{
        std::string url = getMusicUrl(id);
        result = MessageManager::buildMsg("record", url);
        Logger::debug("消息构造成功", result);
    }
    return result;
}