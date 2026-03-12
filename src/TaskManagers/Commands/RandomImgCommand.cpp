#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

std::string RandomImgCommand::getImgUrl()
{
    httplib::SSLClient cli(RANDOM_IMG_HOST, RANDOM_IMG_PORT);
    auto res = cli.Get(RANDOM_IMG_GET_PATH);
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
    std::string img_url = data["url"].get<std::string>();
    return img_url;
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
    return MessageManager::buildMsg("image", getImgUrl());
}   
