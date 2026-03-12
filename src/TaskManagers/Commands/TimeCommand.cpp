#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"

// 获取当前时间的函数
std::string TimeCommand::getFormattedTime() 
{ 
    time_t now = time(0);
    tm local_time;
    if(localtime_r(&now, &local_time) == nullptr)
    {
        Logger::error("获取本地时间失败", "");
        return "时间获取失败";
    }
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_time);
    return std::string(buffer);
}

std::string TimeCommand::name()
{
    return "时间";
}

json TimeCommand::execute(const std::string& args)
{
    return MessageManager::buildMsg("text", getFormattedTime());
}
