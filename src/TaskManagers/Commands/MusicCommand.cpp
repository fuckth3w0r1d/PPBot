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
    json data = json::parse(res->body);
    std::string id = data["result"]["songs"][0]["id"].get<std::string>();
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
    json data = json::parse(res->body);
    std::string url = data[0]["url"].get<std::string>();
    return url;
}

std::string MusicCommand::name() 
{
    return "点歌";
}

json MusicCommand::execute(const std::string& args)
{
    json result = json::array();
    std::string id = getMusicId(args);
    if(id.empty())
    {
        result.emplace_back(MessageManager::buildMsg("text", "搜索取歌曲失败"));
    }else{
        result.emplace_back(MessageManager::buildMsg("record", getMusicUrl(id)));
    }
}