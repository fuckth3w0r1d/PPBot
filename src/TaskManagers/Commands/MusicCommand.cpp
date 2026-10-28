#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

std::string MusicCommand::getMusicId(const std::string& tag)
{
    httplib::Client cli(MUSIC_HOST, MUSIC_PORT);
    auto res = cli.Get(MUSIC_GET_ID_PATH + "?keywords=" + tag);
    if(!res)
    {
        Logger::warn("歌曲搜索 网络请求失败: ", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 200)
    {
        Logger::httpError("歌曲搜索", res->status, res->body);
        return "";
    }
    try
    {
        const auto data = json::parse(res->body);
        return std::to_string(data.at("result").at("songs").at(0).at("id").get<size_t>());
    }
    catch(const json::exception& error)
    {
        Logger::httpError("歌曲搜索", res->status, res->body, std::string("响应解析失败: ") + error.what());
        return "";
    }
}

std::string MusicCommand::getMusicUrl(const std::string& id)
{
    httplib::Client cli(MUSIC_HOST, MUSIC_PORT);
    auto res = cli.Get(MUSIC_GET_URL_PATH + "?id=" + id + "&level=exhigh");
    if(!res)
    {
        Logger::warn("歌曲直链 网络请求失败: ", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 200)
    {
        Logger::httpError("歌曲直链", res->status, res->body);
        return "";
    }
    try
    {
        const auto data = json::parse(res->body);
        const auto url = data.at("data").at(0).at("url").get<std::string>();
        if(url.empty()) Logger::httpError("歌曲直链", res->status, res->body, "响应未提供歌曲地址");
        return url;
    }
    catch(const json::exception& error)
    {
        Logger::httpError("歌曲直链", res->status, res->body, std::string("响应解析失败: ") + error.what());
        return "";
    }
}

std::string MusicCommand::name()
{
    return "点歌";
}

json MusicCommand::execute(const std::string& args)
{
    if(args.empty()) return MessageManager::buildMsg("text", "请输入歌曲名称");
    const auto id = getMusicId(args);
    if(id.empty()) return MessageManager::buildMsg("text", "搜索歌曲失败");
    const auto url = getMusicUrl(id);
    if(url.empty()) return MessageManager::buildMsg("text", "获取歌曲地址失败");
    return MessageManager::buildMsg("record", url);
}
