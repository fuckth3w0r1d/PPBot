#pragma once

#include <string>
#include <json.hpp>

#include "ToolManagers/MessageManager.h"

using json = nlohmann::json;

//////////
// 文本指令接口
//////////
class Command{
public:
    virtual std::string name() = 0;
    virtual std::string sendType()
    { // 默认直接发送
        return "direct";
    }
    virtual json execute(const std::string& args) = 0;
    virtual ~Command() = default;
};
 

///////////
// 各个文本指令
///////////
// 帮助
class HelpCommand : public Command{
private:
    std::string cmd_list;
public:
    HelpCommand(std::string cl);
    std::string name() override;
    json execute(const std::string& args) override;
};
// 时间
class TimeCommand : public Command{
private:
    // 获取当前时间的函数
    std::string getFormattedTime();
public:
    std::string name() override;
    json execute(const std::string& args) override;
};
// 天气
class WeatherCommand : public Command{
private:
    // 简单提取城市名称
    std::string getCityName(const std::string& args);

    // 天气信息结构体
    struct Weatherinfo{
        std::string city; // 城市
        std::string weather; // 天气
        std::string temper; // 温度
        std::string winddir; // 风向
        std::string windpow; // 风力
        std::string humidity; // 湿度
        std::string reporttime; // 查询时间
    };
    // 获取天气信息
    Weatherinfo getWeatherinfo(const json& raw_data);
    // 访问Amap api获取信息
    std::string askAmap(const std::string& args);
public:
    std::string name() override;
    json execute(const std::string& args) override;
};

// 随机二次元图片
class RandomImgCommand : public Command{
private:
    std::string getImgUrl();

public:
    std::string name() override;
    std::string sendType() override; // 改用转发
    json execute(const std::string& args) override; 
};

// 吃啥
class MealCommand : public Command{
private:
    std::string getMealWhat();

public:
    std::string name() override;
    json execute(const std::string& args) override;
};

// 随机图库图片
class RandomImgCommand2 : public Command{
private:
    std::vector<std::string> getImgUrls();

public:
    std::string name() override;
    std::string sendType() override; // 改用转发
    json execute(const std::string& args) override; 
};