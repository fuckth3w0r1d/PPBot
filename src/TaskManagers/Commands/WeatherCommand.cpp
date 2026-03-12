#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

// 简单提取城市名称
std::string WeatherCommand::getCityName(const std::string& args)
{
    if (args.empty()) 
    {
        return "";
    }
    std::string city = args;
    // 去除可能的前置空格
    while(!city.empty() && city[0] == ' ')
    {
        city.erase(0, 1);
    }
    // 去除可能的末尾空格
    while (!city.empty() && city.back() == ' ')
    {
        city.pop_back();
    }
    // 只取第一个城市名称
    size_t pos = city.find(' ');
    if (pos != std::string::npos) 
    {
        city = city.substr(0, pos);
    }
    return city;
}

// 获取天气信息
WeatherCommand::Weatherinfo WeatherCommand::getWeatherinfo(const json& raw_data)
{
    WeatherCommand::Weatherinfo result;
    const json& data = raw_data["lives"][0];
    result.city = data["province"].get<std::string>() + " "
        + data["city"].get<std::string>();
    result.weather = data["weather"].get<std::string>();
    result.temper = data["temperature"].get<std::string>();
    result.winddir = data["winddirection"].get<std::string>();
    result.windpow = data["windpower"].get<std::string>();
    result.humidity = data["humidity"].get<std::string>();
    result.reporttime = data["reporttime"].get<std::string>();
    return result;
}
// 访问Amap api获取信息
std::string WeatherCommand::askAmap(const std::string& args)
{
    std::string city = getCityName(args);
    if(city.empty())
    {
        return "请输入城市名称, 格式: 天气 城市名称";
    }
    httplib::SSLClient cli(AMAP_HOST, AMAP_PORT);
    auto res = cli.Get(AMAP_GET_PATH + "?city=" + city + "&key=" + AMAP_KEY);
    if(!res)
    {
        Logger::error("天气网络请求失败", httplib::to_string(res.error()));
        return "天气网络请求失败";
    }
    if(res->status != 200)
    {
        Logger::warn("天气请求 HTTP状态码: ", res->status);
        Logger::error("天气请求 异常响应体:", json::parse(res->body).dump(4));
        return "天气请求异常";
    }
    // 先解析返回的JSON
    json raw_data = json::parse(res->body);
    if(raw_data["lives"].empty())
    {
        Logger::warn("天气查询返回异常", raw_data.dump(4));
        return "天气查询失败, 请输入正确的格式: 天气 城市名称\n暂时只支持国内城市";
    }
    WeatherCommand::Weatherinfo winfo = getWeatherinfo(raw_data);
    std::string result = "城市: " + winfo.city;
    result += "\n天气: " + winfo.weather;
    result += "\n温度: " + winfo.temper + "℃";
    result += "\n风向: " + winfo.winddir + "风 " + winfo.windpow + "级";
    result += "\n湿度: " + winfo.humidity + "%";
    result += "\n查询时间: " + winfo.reporttime;
    return result;
}

std::string WeatherCommand::name()
{
    return "天气";
}
json WeatherCommand::execute(const std::string& args)
{
    return MessageManager::buildMsg("text", askAmap(args));
}
