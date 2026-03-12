#include "TaskManagers/Commands.h"
#include "logger.h"
#include "config.h"


HelpCommand::HelpCommand(std::string cl)
{
    cmd_list = cl;
}

std::string HelpCommand::name()
{
    return "帮助";
}

json HelpCommand::execute(const std::string& args)
{
    // 返回维护的指令列表
    return MessageManager::buildMsg("text", "指令格式: @我 指令\n当前支持的指令:\n" + cmd_list);
}

