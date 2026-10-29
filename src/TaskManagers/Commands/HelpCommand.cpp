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
    return MessageManager::buildMsg("text", "请显式 @ 本机器人并使用 /命令。推荐格式: @机器人 /命令 参数\n当前支持的指令:\n" + cmd_list);
}

