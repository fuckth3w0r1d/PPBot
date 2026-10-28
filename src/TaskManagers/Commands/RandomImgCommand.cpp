#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

std::string RandomImgCommand::getImgUrl()
{
    httplib::SSLClient cli(RANDOM_IMG_HOST, RANDOM_IMG_PORT);
    auto res = cli.Get(RANDOM_IMG_GET_PATH);
    if(!res)
    {
        Logger::warn("随机图片 网络请求失败: ", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 200)
    {
        Logger::httpError("随机图片", res->status, res->body);
        return "";
    }
    try
    {
        const auto data = json::parse(res->body);
        const auto url = data.at("url").get<std::string>();
        if(url.empty()) Logger::httpError("随机图片", res->status, res->body, "响应未提供图片地址");
        return url;
    }
    catch(const json::exception& error)
    {
        Logger::httpError("随机图片", res->status, res->body, std::string("响应解析失败: ") + error.what());
        return "";
    }
}

std::string RandomImgCommand::name()
{
    return "随机二次元图片";
}

std::string RandomImgCommand::sendType()
{
    return "forward";
}

json RandomImgCommand::execute(const std::string& args)
{
    const auto url = getImgUrl();
    return MessageManager::buildMsg(url.empty() ? "text" : "image", url.empty() ? "随机图片获取失败" : url);
}
