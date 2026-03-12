#include "TaskManagers/TaskManagers.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

//////////////
// 总的任务管理器
//////////////

std::vector<std::unique_ptr<BaseTaskManager>> tsk_managers;
void TaskManager::registerTaskManager(std::unique_ptr<BaseTaskManager> tsk_manager)
{
    tsk_managers.emplace_back(std::move(tsk_manager));
}


TaskManager::TaskManager()
{
    // 注册特定任务管理器
    registerTaskManager(std::make_unique<CmdTaskManager>());
    registerTaskManager(std::make_unique<ChatTaskManager>()); // 注意这里AI任务优先级低于命令任务
    registerTaskManager(std::make_unique<JsonTaskManager>());
}

bool TaskManager::canHandle(const MessageContext& msgctx)
{
    return true;
}


// 总的任务处理函数
json TaskManager::handleTask(const MessageContext& msgctx)
{
    for(auto& tsk_manager : tsk_managers)
    {
        if(tsk_manager->canHandle(msgctx))
        {
            // 按照能否处理自动分发任务处理器
            return tsk_manager->handleTask(msgctx);
        }
    }
    return {};
}

