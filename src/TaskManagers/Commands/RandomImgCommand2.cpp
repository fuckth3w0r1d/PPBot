#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

std::vector<std::string> RandomImgCommand2::getImgUrls()
{
    httplib::SSLClient cli(RANDOM_IMG_HOST2, RANDOM_IMG_PORT);
    auto res = cli.Get(RANDOM_IMG_GET_PATH2);
    if(!res)
    {
        Logger::error("随机图片网络请求失败", httplib::to_string(res.error()));
        return {};
    }
    if(res->status != 200)
    {
        Logger::warn("随机图片请求 HTTP状态码: ", res->status);
        Logger::error("随机图片请求 异常响应体:", json::parse(res->body).dump(4));
        return {};
    }
    json data = json::parse(res->body);
    std::vector<std::string> img_urls = data["proxyUrls"].get<std::vector<std::string>>();
    return img_urls;
}


std::string RandomImgCommand2::name()
{
    return "来点涩图";
}

std::string RandomImgCommand2::sendType()
{
    return "forward";
}

json RandomImgCommand2::execute(const std::string& args)
{
    std::vector<std::string> img_urls = getImgUrls();
    json result = json::array();
    for(const auto& url : img_urls)
    {
        result.emplace_back(MessageManager::buildMsg("image", url));
    }
    return result;
}   
