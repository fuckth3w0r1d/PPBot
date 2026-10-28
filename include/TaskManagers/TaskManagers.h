#pragma once

#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>

#include "ToolManagers/MessageManager.h"
#include "ToolManagers/FileManager.h"
#include "TaskManagers/Commands.h"

using json = nlohmann::json;

///////////
// 任务管理器接口
///////////
class BaseTaskManager{
public:
    virtual bool canHandle(const MessageContext& msgctx) = 0;
    virtual std::pair<json, std::string> handleTask(const MessageContext& msgctx) = 0;
    virtual ~BaseTaskManager() = default;
};

////////////
// 文本命令任务管理器
////////////
class CmdTaskManager : public BaseTaskManager{
private: 
    // 维护一个指令表，用于存储多种可支持的指令
    static std::unordered_map<std::string, std::unique_ptr<Command>> cmd_map;
    // 注册一个用指令接口实现的指令
    void registerCommand(std::unique_ptr<Command> cmd);

public:
    CmdTaskManager();
    // 用于获取指令列表
    std::string getCommandList();
    // 能否处理
    bool canHandle(const MessageContext& msgctx) override;
    // 处理某个被at的文本指令
    std::pair<json, std::string> handleTask(const MessageContext& msgctx) override;
};


//////////////
// JSON消息任务管理器
//////////////
class JsonTaskManager : public BaseTaskManager{
private:
    // B站视频信息结构
    struct BVinfo{
        std::string title;
        std::string bvid;
        std::string cid; // 获取视频需要的请求参数
        std::string up; // up主昵称
        std::string face; // 封面url
        std::string url; // 视频URL
        size_t size; // 视频大小
        int view; // 观看次数
        int reply; // 评论数
        int favorite; // 收藏数
        int coin; // 投币数
        int share; // 分享数
        int like; // 点赞数
    };
    // 获取 bvid
    std::string getBVid(const json& data);
    // 获取B站视频信息
    BVinfo getBVinfo(const json& raw_data);
    // 获取B站视频直链url和视频大小
    void getBVUrlandSize(const std::string& bvid, const std::string& cid, BVinfo& bvinfo);
    
    // 处理B站视频
    json handleBV(const json& data);
public:
    bool canHandle(const MessageContext& msgctx) override;
    std::pair<json, std::string> handleTask(const MessageContext& msgctx) override;
};

// AI对话任务
class ChatTaskManager : public BaseTaskManager{
private:
    std::mutex state_mutex;
    std::condition_variable timer_condition;
    std::thread timer;
    bool running = true;
    uint64_t next_sequence = 0;
    uint64_t state_version = 0;
    uint64_t saved_version = 0;
    json group_timeline = json::object();
    json persona_map = json::object();
    json users_profile_map = json::object();
    json session_round_counter = json::object();
    json group_round_counter = json::object();
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> request_mutexes;

    void loadData();
    bool saveData();
    json buildMessages(const MessageContext& msgctx, bool judge, const std::string& purpose);
    void updateSummaries(const MessageContext& msgctx);

public:
    ChatTaskManager();
    void recordIncoming(MessageContext& msgctx);
    void recordOutgoing(const MessageContext& recv, const json& segments, const std::string& message_id, bool is_poke = false);
    bool canHandle(const MessageContext& msgctx) override;
    std::pair<json, std::string> handleTask(const MessageContext& msgctx) override;
    ~ChatTaskManager();
};


//////////////
// 总的任务管理器
//////////////
class TaskManager : public BaseTaskManager{
private:
    std::vector<std::unique_ptr<BaseTaskManager>> tsk_managers;
    ChatTaskManager* chat_manager = nullptr;
    void registerTaskManager(std::unique_ptr<BaseTaskManager> tsk_manager);

public:
    TaskManager();
    bool canHandle(const MessageContext& msgctx) override;
    // 总的任务处理函数
    std::pair<json, std::string> handleTask(const MessageContext& msgctx) override;
    void recordOutgoing(const MessageContext& recv, const SentMessage& sent);
};
