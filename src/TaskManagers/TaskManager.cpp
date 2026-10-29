#include "TaskManagers/TaskManagers.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

//////////////
// 总的任务管理器
//////////////

void TaskManager::registerTaskManager(std::unique_ptr<BaseTaskManager> tsk_manager)
{
    tsk_managers.emplace_back(std::move(tsk_manager));
}


TaskManager::TaskManager()
{
    // 注册特定任务管理器
    registerTaskManager(std::make_unique<CmdTaskManager>());
    registerTaskManager(std::make_unique<JsonTaskManager>());
    auto chat = std::make_unique<ChatTaskManager>();
    chat_manager = chat.get();
    registerTaskManager(std::move(chat));
}

bool TaskManager::canHandle(const MessageContext& msgctx)
{
    return true;
}


// 总的任务处理函数
std::pair<json, std::string> TaskManager::handleTask(const MessageContext& msgctx)
{
    auto context = msgctx;
    chat_manager->recordIncoming(context);
    if(context.msg_type != "group" || context.user_id == BOT_QQ) return {};
    if(context.is_poke) return chat_manager->handleTask(context);
    for(auto& tsk_manager : tsk_managers)
    {
        if(tsk_manager->canHandle(context))
        {
            // 按照能否处理自动分发任务处理器
            return tsk_manager->handleTask(context);
        }
    }
    return {};
}

void TaskManager::recordOutgoing(const MessageContext& recv, const SentMessage& sent)
{
    chat_manager->recordOutgoing(recv, sent.segments, sent.message_id, sent.is_poke);
}

bool TaskManager::shutdown()
{
    return chat_manager->shutdown();
}

