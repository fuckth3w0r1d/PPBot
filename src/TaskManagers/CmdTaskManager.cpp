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
    std::string cmd_list = "/帮助\n";
    for(const auto& cmd : cmd_map)
    {
        if(cmd.first != "帮助") cmd_list += "/" + cmd.first + "\n";
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
    if(msgctx.msg_type != "group" || msgctx.user_id == BOT_QQ || !msgctx.pmsgsegs.at_me) return false;
    if(msgctx.pmsgsegs.text.empty() || msgctx.pmsgsegs.text[0] != '/') return false;
    size_t pos = msgctx.pmsgsegs.text.find(' ');
    std::string cmd_name = msgctx.pmsgsegs.text.substr(1, pos == std::string::npos ? pos : pos - 1);
    return cmd_map.count(cmd_name);
}

// 处理某个文本指令
std::pair<json, std::string> CmdTaskManager::handleTask(const MessageContext& msgctx)
{
    if(!canHandle(msgctx)) return {};
    size_t pos = msgctx.pmsgsegs.text.find(' ');
    std::string cmd_name = msgctx.pmsgsegs.text.substr(1, pos == std::string::npos ? pos : pos - 1);
    std::string cmd_args = (pos == std::string::npos) ? "" : msgctx.pmsgsegs.text.substr(pos + 1);
    cmd_args.erase(0, cmd_args.find_first_not_of(' '));
    auto& command = cmd_map.at(cmd_name);
    json result = command->execute(cmd_args);
    if(!result.is_array()) result = json::array({result});
    const auto sendType = command->sendType();
    if(sendType == "direct" && result.size() == 1 && result.front().value("type", "") == "text" && !msgctx.message_id.empty())
        result.insert(result.begin(), MessageManager::buildMsg("reply", msgctx.message_id));
    return std::make_pair(result, sendType);
}

