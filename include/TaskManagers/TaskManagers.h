#pragma once

#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>

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
    virtual json handleTask(const MessageContext& msgctx) = 0;
    virtual ~BaseTaskManager() = default;
};

////////////
// 被at的文本命令任务管理器
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
    json handleTask(const MessageContext& msgctx) override;
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
        std::string face; // 头像url
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
    std::pair<std::string, std::string> handleBV(const json& data);
public:
    bool canHandle(const MessageContext& msgctx) override;
    json handleTask(const MessageContext& msgctx) override;
};

// AI对话任务
class ChatTaskManager : public BaseTaskManager{
private:
    // 记录每个 session 对话轮数
    static std::unordered_map<std::string, size_t> session_round_counter;
    static std::shared_mutex session_round_counter_mutex;
    // 记录每个群聊对话轮数
    static std::unordered_map<std::string, size_t> group_round_counter;
    static std::shared_mutex group_round_counter_mutex;
    // 保存 session 对话轮数到数据文件
    static bool saveSessionRoundCounter();
    // 加载 session 对话轮数
    bool loadSessionRoundCounter();

    // 保存群聊对话轮数到数据文件
    static bool saveGroupRoundCounter();
    // 加载群聊对话轮数
    bool loadGroupRoundCounter();

    // AI 会话历史结构体
    struct SessionMemCtx{
        std::string role;
        std::string content;
    };
    // 每个 session 对话历史保存
    static std::unordered_map<std::string, std::vector<SessionMemCtx>> session_memory;
    // 会话历史全局锁
    static std::shared_mutex session_memory_global_lock;
    // 针对每个 session 的锁表
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> session_mutex_map;
    static std::mutex session_mutex_map_lock;
    // 获取对话 session id
    std::string getSessionId(const MessageContext& msgctx);
    // 获取或者创建 session 对应的锁
    std::shared_ptr<std::mutex> getSessionMutex(const std::string& session_id);
    // 获取或者创建 session 对应的对话历史
    std::vector<SessionMemCtx> getSessionHistory(const std::string& session_id);
    // 获取某个群聊内所有留存的对话历史
    std::vector<SessionMemCtx> getGroupSessionHistory(const std::string& group_id);
    // 更新会话历史
    bool UpdateSessionMemory(const std::string& session_id, const std::string& user_input, const std::string& ai_reply);
    // 保存 session memory 到数据文件中
    static bool saveSessionMemory();
    // 从数据文件中加载 session memory
    bool loadSessionMemory();

    // bot 的人格记忆
    std::string learned_persona;  // 用户影响产生的人格
    // 不同群聊的人格记忆隔离, 用一个表记录
    static std::unordered_map<std::string, std::string> persona_map;
    // 全局锁
    static std::shared_mutex persona_map_global_lock;
    // 锁表
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> persona_mutex_map;
    static std::mutex persona_mutex_map_lock;
    // 获取或者创建群聊人格对应的锁
    std::shared_ptr<std::mutex> getPersonaMutex(const std::string& group_id);
    // 获取或者创建对应群聊的人格记忆
    std::string getBotPersona(const std::string& group_id);
    // 更新对应群聊的人格记忆
    bool UpdateBotPersona(const std::string& group_id);
    // 保存 bot 人格到数据文件中
    static bool saveBotPersona();
    // 从数据文件中加载 bot 人格
    bool loadBotPersona();

    // 给对话过的用户画像
    struct UserProfile{
        std::string nickname; // 昵称
        std::string card; // 群昵称
        std::string description;   // AI总结的人设
    };
    // 不同群聊用户画像的记录
    static std::unordered_map<std::string, std::unordered_map<std::string, UserProfile>> users_profile_map;
    // 一个全局锁
    static std::shared_mutex users_profile_map_global_lock;
    // 群聊用户画像总的锁表
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> users_profile_mutex_map;
    static std::mutex users_profile_mutex_map_lock;
    // 获取或创建群聊用户画像锁
    std::shared_ptr<std::mutex> getUsersProMutex(const std::string& group_id);
    // 获取或创建群聊用户画像
    std::unordered_map<std::string, UserProfile> getUsersProfile(const std::string& group_id, const std::string& user_id);
    // 更新群聊用户画像
    bool UpdateUserProfile(const std::string& session_id, const std::string& user_id, const std::string& group_id);
    // 保存用户画像到数据文件中
    static bool saveUsersProfile();
    // 从数据文件中加载用户画像
    bool loadUserProfile();


    //与chatAI交互
    std::string ChatWithAI(const MessageContext& msgctx);
    // 单纯的与纯净模型交互 (豆包)
    std::string askAI(const MessageContext& msgctx);
    // 定时自动保存数据
    static bool autoSaveData();
    // 数据脏位
    static std::atomic<bool> dirty_bit;
    // 计时
    static std::thread timer;
    static std::atomic<bool> running;

public:
    ChatTaskManager();
    bool canHandle(const MessageContext& msgctx) override;
    json handleTask(const MessageContext& msgctx) override;
    ~ChatTaskManager();
};


//////////////
// 总的任务管理器
//////////////
class TaskManager : public BaseTaskManager{
private:
    std::vector<std::unique_ptr<BaseTaskManager>> tsk_managers;
    void registerTaskManager(std::unique_ptr<BaseTaskManager> tsk_manager);

public:
    TaskManager();
    bool canHandle(const MessageContext& msgctx) override;
    // 总的任务处理函数
    json handleTask(const MessageContext& msgctx) override;
};
