#include "TaskManagers/TaskManagers.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

////////////
// 被at的文本命令任务管理器
////////////

// 维护一个指令表，用于存储多种可支持的指令
std::unordered_map<std::string, std::unique_ptr<Command>> CmdTaskManager::cmd_map;
// 注册一个用指令接口实现的指令
void CmdTaskManager::registerCommand(std::unique_ptr<Command> cmd)
{ 
    cmd_map[cmd->name()] = std::move(cmd);
}


// 用于获取指令列表
std::string CmdTaskManager::getCommandList()
{
    std::string cmd_list = "帮助\n"; // 初始 list 带有帮助指令
    for(const auto& cmd : cmd_map)
    {
        cmd_list += cmd.first + "\n"; 
    }
    // 去除最后一个回车符
    cmd_list.pop_back();
    return cmd_list;
}

CmdTaskManager::CmdTaskManager()
{
    // 注册各个指令
    registerCommand(std::make_unique<TimeCommand>());
    registerCommand(std::make_unique<WeatherCommand>());
    registerCommand(std::make_unique<RandomImgCommand>());
    registerCommand(std::make_unique<RandomImgCommand2>());
    registerCommand(std::make_unique<MealCommand>());
    registerCommand(std::make_unique<MusicCommand>());
    // 后续文本指令也在此注册
    registerCommand(std::make_unique<HelpCommand>(getCommandList()));
}

// 能否处理
bool CmdTaskManager::canHandle(const MessageContext& msgctx)
{
    // 仅能处理群聊的消息
    if(msgctx.msg_type != "group") return false;
    if(msgctx.pmsgsegs.text.empty()) return false;
    // 先按照空格分割指令名称和参数（解析text时已经去除了前置空格）
    size_t pos = msgctx.pmsgsegs.text.find(' ');
    std::string cmd_name = msgctx.pmsgsegs.text.substr(0, pos);
    return cmd_map.count(cmd_name); // 仅能处理指令表中存在的指令
}

// 处理某个文本指令
std::pair<json, std::string> CmdTaskManager::handleTask(const MessageContext& msgctx)
{
    // 执行指令
    // 先按照空格分割指令名称和参数
    size_t pos = msgctx.pmsgsegs.text.find(' ');
    std::string cmd_name = msgctx.pmsgsegs.text.substr(0, pos);
    // 去除可能的前置空格
    while(!cmd_name.empty() && cmd_name[0] == ' ')
    {
        cmd_name.erase(0, 1);
    }
    std::string cmd_args = (pos == std::string::npos) ? "" : msgctx.pmsgsegs.text.substr(pos + 1);
    // 去除可能的前置空格
    while(!cmd_args.empty() && cmd_args[0] == ' ')
    {
        cmd_args.erase(0, 1);
    }
    json result = json::array();
    std::string sendType;
    if(cmd_map.count(cmd_name))
    {
        // 调用对应指令
        json tmp_result = cmd_map[cmd_name]->execute(cmd_args);
        if(tmp_result.is_array())
        {
            result = tmp_result;
        }else{
            result.emplace_back(tmp_result);
        }
        return std::make_pair(result, cmd_map[cmd_name]->sendType());
    }
    result.emplace_back(MessageManager::buildMsg("text", "未知指令: " + cmd_name));
    return std::make_pair(result, "direct");
}

