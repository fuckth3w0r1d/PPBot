#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

std::vector<std::string> RandomImgCommand2::getImgUrls(const std::string& tag)
{
    httplib::Client cli(RANDOM_IMG_HOST2, RANDOM_IMG_PORT2);
    auto res = cli.Get(RANDOM_IMG_GET_PATH2 + "?tag=" + tag);
    if(!res)
    {
        Logger::warn("标签图片 网络请求失败: ", httplib::to_string(res.error()));
        return {};
    }
    if(res->status != 200)
    {
        Logger::httpError("标签图片", res->status, res->body);
        return {};
    }
    try
    {
        const auto data = json::parse(res->body);
        auto urls = data.at("proxyUrls").get<std::vector<std::string>>();
        if(urls.empty())
        {
            Logger::httpError("标签图片", res->status, res->body, "响应未提供图片地址");
            return {};
        }
        for(const auto& url : urls)
            if(url.empty())
            {
                Logger::httpError("标签图片", res->status, res->body, "响应包含空图片地址");
                return {};
            }
        return urls;
    }
    catch(const json::exception& error)
    {
        Logger::httpError("标签图片", res->status, res->body, std::string("响应解析失败: ") + error.what());
        return {};
    }
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
    const auto urls = getImgUrls(args);
    json result = json::array();
    if(urls.empty()) result.push_back(MessageManager::buildMsg("text", "标签图片获取失败"));
    for(const auto& url : urls) result.push_back(MessageManager::buildMsg("image", url));
    return result;
}
