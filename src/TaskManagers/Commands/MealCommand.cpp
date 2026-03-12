#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"


std::string MealCommand::getMealWhat()
{
    httplib::SSLClient cli(EAT_HOST, EAT_PORT);
    auto res = cli.Get(EAT_GET_PATH);
    if(!res)
    {
        Logger::error("今天吃啥api网络请求失败", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 200)
    {
        Logger::warn("今天吃啥api请求 HTTP状态码: ", res->status);
        Logger::error("今天吃啥api请求 异常响应体:", json::parse(res->body).dump(4));
        return "";
    }
    json data = json::parse(res->body);
    std::string result = data["mealwhat"].get<std::string>();
    return result;
}


std::string MealCommand::name() 
{
    return "吃啥";
}

json MealCommand::execute(const std::string& args) 
{
    return MessageManager::buildMsg("text", getMealWhat());
}
