#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

std::string MealCommand::getMealWhat()
{
    httplib::SSLClient cli(EAT_HOST, EAT_PORT);
    auto res = cli.Get(EAT_GET_PATH);
    if(!res)
    {
        Logger::warn("餐食查询 网络请求失败: ", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 200)
    {
        Logger::httpError("餐食查询", res->status, res->body);
        return "";
    }
    try
    {
        const auto data = json::parse(res->body);
        const auto meal = data.at("mealwhat").get<std::string>();
        if(meal.empty()) Logger::httpError("餐食查询", res->status, res->body, "响应未提供餐食内容");
        return meal;
    }
    catch(const json::exception& error)
    {
        Logger::httpError("餐食查询", res->status, res->body, std::string("响应解析失败: ") + error.what());
        return "";
    }
}

std::string MealCommand::name()
{
    return "吃啥";
}

json MealCommand::execute(const std::string& args)
{
    const auto meal = getMealWhat();
    return MessageManager::buildMsg("text", meal.empty() ? "餐食查询失败" : meal);
}
