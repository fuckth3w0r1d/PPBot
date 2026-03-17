#include "TaskManagers/TaskManagers.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

std::unordered_map<std::string, std::vector<ChatTaskManager::SessionMemCtx>> ChatTaskManager::session_memory;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> ChatTaskManager::session_mutex_map;
std::mutex ChatTaskManager::session_mutex_map_lock;
std::shared_mutex ChatTaskManager::session_memory_global_lock;

std::unordered_map<std::string, std::string> ChatTaskManager::persona_map;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> ChatTaskManager::persona_mutex_map;
std::mutex ChatTaskManager::persona_mutex_map_lock;
std::shared_mutex ChatTaskManager::persona_map_global_lock;

std::unordered_map<std::string, std::unordered_map<std::string, ChatTaskManager::UserProfile>> ChatTaskManager::users_profile_map;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> ChatTaskManager::users_profile_mutex_map;
std::mutex ChatTaskManager::users_profile_mutex_map_lock;
std::shared_mutex ChatTaskManager::users_profile_map_global_lock;

std::unordered_map<std::string, size_t> ChatTaskManager::session_round_counter;
std::shared_mutex ChatTaskManager::session_round_counter_mutex;
std::unordered_map<std::string, size_t> ChatTaskManager::group_round_counter;
std::shared_mutex ChatTaskManager::group_round_counter_mutex;

std::atomic<bool> ChatTaskManager::dirty_bit;
std::atomic<bool> ChatTaskManager::running;
std::thread ChatTaskManager::timer;

// 保存 session 对话轮数到数据文件
bool ChatTaskManager::saveSessionRoundCounter()
{
    json data;
    std::shared_lock<std::shared_mutex> rlock(session_round_counter_mutex);
    data["session_round_counter"] = session_round_counter;
    return FileManager::writeJsonFile(DATA_PATH + "/AiModule/session_round_counter.json", data);
}
// 加载 session 对话轮数
bool ChatTaskManager::loadSessionRoundCounter()
{
    json data;
    if(FileManager::readJsonFile(DATA_PATH + "/AiModule/session_round_counter.json", data))
    {
        if(data.empty())
        {
            Logger::warn("从数据文件中加载会话轮数时文件为空: ", DATA_PATH + "/AiModule/session_round_counter.json");
            return true;
        }
        if(!data.contains("session_round_counter"))
        {
            Logger::warn("从数据文件中加载会话轮数时文件异常: ", DATA_PATH + "/AiModule/session_round_counter.json");
            return false;
        }
        std::lock_guard<std::shared_mutex> lock(session_round_counter_mutex);
        session_round_counter.clear();
        session_round_counter = data["session_round_counter"].get<std::unordered_map<std::string, size_t>>();
        return true;
    }
    return false;
}

// 保存群聊对话轮数到数据文件
bool ChatTaskManager::saveGroupRoundCounter()
{
    json data;
    std::shared_lock<std::shared_mutex> rlock(group_round_counter_mutex);
    data["group_round_counter"] = group_round_counter;
    return FileManager::writeJsonFile(DATA_PATH + "/AiModule/group_round_counter.json", data);
}
// 加载群聊对话轮数
bool ChatTaskManager::loadGroupRoundCounter()
{
    json data;
    if(FileManager::readJsonFile(DATA_PATH + "/AiModule/group_round_counter.json", data))
    {
        if(data.empty())
        {
            Logger::warn("从数据文件中加载会话轮数时文件为空: ", DATA_PATH + "/AiModule/group_round_counter.json");
            return true;
        }
        if(!data.contains("group_round_counter"))
        {
            Logger::warn("从数据文件中加载会话轮数时文件异常: ", DATA_PATH + "/AiModule/group_round_counter.json");
            return false;
        }
        std::lock_guard<std::shared_mutex> lock(group_round_counter_mutex);
        group_round_counter.clear();
        group_round_counter = data["group_round_counter"].get<std::unordered_map<std::string, size_t>>();
        return true;
    }
    return false;
}

// 获取对话 session id
std::string ChatTaskManager::getSessionId(const MessageContext& msgctx)
{
    if(msgctx.msg_type == "group")
    {
        return "group_" + msgctx.group_id + "_user_" + msgctx.user_id;
    }else{
        return "private_" + msgctx.user_id;
    }
    return "";
}
// 获取或者创建 session 对应的锁
std::shared_ptr<std::mutex> ChatTaskManager::getSessionMutex(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(session_mutex_map_lock);
    if(session_mutex_map.find(session_id) == session_mutex_map.end())
    {
        session_mutex_map[session_id] = std::make_shared<std::mutex>();
    }
    return session_mutex_map[session_id];
}
// 获取或者创建 session 对应的对话历史
std::vector<ChatTaskManager::SessionMemCtx> ChatTaskManager::getSessionHistory(const std::string& session_id)
{
    // 先上一个session_memory的全局写锁
    std::unique_lock<std::shared_mutex> wlock(session_memory_global_lock);
    if(session_memory.count(session_id))
    {   // 如果已经存在则解全局锁
        wlock.unlock();
    }
    // 获取锁
    std::shared_ptr<std::mutex> session_mutex = getSessionMutex(session_id);
    std::lock_guard<std::mutex> session_lock(*session_mutex);
    return session_memory[session_id];
}
// 获取某个群聊内所有留存的对话历史
std::vector<ChatTaskManager::SessionMemCtx> ChatTaskManager::getGroupSessionHistory(const std::string& group_id)
{
    std::vector<SessionMemCtx> result = {};
    std::vector<std::string> session_ids;
    // 加上全局读锁
    std::shared_lock<std::shared_mutex> rlock(session_memory_global_lock);
    for(auto& session : session_memory)
    {
        const std::string& session_id = session.first;
        session_ids.push_back(session_id);
    }
    rlock.unlock(); // 释放读锁
    for(auto& session_id : session_ids)
    {
        size_t pos = session_id.find(group_id);
        if(pos != std::string::npos)
        {
            // 上锁
            std::shared_ptr<std::mutex> session_mutex = getSessionMutex(session_id);
            std::lock_guard<std::mutex> session_lock(*session_mutex);
            result.insert(result.end(), session_memory[session_id].begin(), session_memory[session_id].end());
        }
    }
    return result;
}
// 更新会话历史
bool ChatTaskManager::UpdateSessionMemory(const std::string& session_id, const std::string& user_input, const std::string& ai_reply)
{
    SessionMemCtx mem1;
    SessionMemCtx mem2;
    mem1.role = "user";
    mem1.content = user_input;
    mem2.role = "assistant";
    mem2.content = ai_reply;
    // 先上锁
    std::shared_ptr<std::mutex> session_mutex = getSessionMutex(session_id);
    std::lock_guard<std::mutex> session_lock(*session_mutex);
    session_memory[session_id].emplace_back(mem1);
    session_memory[session_id].emplace_back(mem2);
    // 如果历史过多则清理历史并设置返回标志来更新用户画像
    if(session_memory[session_id].size() > AI_MAX_CHAT_ROUNDS * 2) // 每轮两条对话
    {
        session_memory[session_id].erase(session_memory[session_id].begin(), session_memory[session_id].begin()+2);
    }
    return true;
}
// 保存 session memory 到数据文件中
bool ChatTaskManager::saveSessionMemory()
{
    json data;
    // 上读锁
    std::shared_lock<std::shared_mutex> rlock(session_memory_global_lock);
    for(auto& [session_id, history] : session_memory)
    {
        for(auto& msg : history)
        {
            data["session_memory"][session_id].push_back({
                {"role", msg.role},
                {"content", msg.content}
            });
        }
    }
    return FileManager::writeJsonFile(DATA_PATH + "/AiModule/session_memory.json", data);
}
// 从数据文件中加载 session memory
bool ChatTaskManager::loadSessionMemory()
{
    json data;
    if(FileManager::readJsonFile(DATA_PATH + "/AiModule/session_memory.json", data))
    {
        if(data.empty())
        {
            Logger::warn("从数据文件中加载 session 时文件为空: ", DATA_PATH + "/AiModule/session_memory.json");
            return true;
        }
        if(!data.contains("session_memory"))
        {
            Logger::warn("从数据文件中加载 session 时文件异常: ", DATA_PATH + "/AiModule/session_memory.json");
            return false;
        }
        std::lock_guard<std::shared_mutex> lock(session_memory_global_lock);
        session_memory.clear();
        for(auto& [session_id, history] : data["session_memory"].items())
        {
            for(auto& mem : history)
            {
                SessionMemCtx ctx;
                ctx.role = mem["role"];
                ctx.content = mem["content"];
                session_memory[session_id].push_back(ctx);
            }
        }
        return true;
    }
    return false;
}

// 获取或者创建群聊人格对应的锁
std::shared_ptr<std::mutex> ChatTaskManager::getPersonaMutex(const std::string& group_id)
{
    std::lock_guard<std::mutex> lock(persona_mutex_map_lock);
    if(persona_mutex_map.find(group_id) == persona_mutex_map.end())
    {
        persona_mutex_map[group_id] = std::make_shared<std::mutex>();
    }
    return persona_mutex_map[group_id];
}
// 获取或者创建对应群聊的人格记忆
std::string ChatTaskManager::getBotPersona(const std::string& group_id)
{
    // 先上一个全局写锁
    std::unique_lock<std::shared_mutex> wlock(persona_map_global_lock);
    if(persona_map.count(group_id))
    {
        wlock.unlock();
    }
    // 上锁
    std::shared_ptr<std::mutex> persona_mutex = getPersonaMutex(group_id);
    std::lock_guard<std::mutex> persona_lock(*persona_mutex);
    return persona_map[group_id];
}
// 更新对应群聊的人格记忆
bool ChatTaskManager::UpdateBotPersona(const std::string& group_id)
{
    // 让 AI 更新群聊人格记忆
    httplib::SSLClient cli(AI_HOST, AI_PORT);
    json body;
    httplib::Headers headers = {
        {"Authorization", "Bearer " + AI_KEY},
        {"Content-Type", "application/json"}
    };
    json messages = json::array();
    messages.push_back({
        {"role", "system"},
        {"content", "现在你为我完成一个任务：以下是某个聊天机器人目前的人格" + getBotPersona(group_id) + "接下来输入一系列user和这个assitant的对话历史(不是和你), 请你融合并生成新的100字人格, 但不要涉及名字服装等等私人化定制内容, 返回给我新的机器人格描述"}
    });
    // 获取群聊的留存对话历史
    std::vector<SessionMemCtx> history = getGroupSessionHistory(group_id);
    // 输入对话历史
    for(auto& msg : history)
    {
        messages.push_back({
            {"role", msg.role},
            {"content", msg.content}
        });
    }
    // 构造请求体
    body["model"] = AI_MODEL;
    body["messages"] = messages;
    body["max_tokens"] = AI_MAX_TOKENS;
    auto res = cli.Post(AI_POST_PATH, headers, body.dump(), "application/json");
    if(!res)
    {
        Logger::error("AI网络请求失败", httplib::to_string(res.error()));
        return false;
    }
    if(res->status != 200)
    {
        Logger::warn("AI请求 HTTP状态码: ", res->status);
        Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
        return false;
    }
    json raw_data = json::parse(res->body);
    if (!raw_data.contains("choices") || raw_data["choices"].empty())
    {
        Logger::warn("AI回复异常", raw_data.dump(4));
        return false;
    }
    std::string new_persona = raw_data["choices"][0]["message"]["content"].get<std::string>();
    // 上锁
    std::shared_ptr<std::mutex> persona_mutex = getPersonaMutex(group_id);
    std::lock_guard<std::mutex> persona_lock(*persona_mutex);
    // 更新bot记忆
    persona_map[group_id] = new_persona;
    return true;
}
// 保存 bot 人格到数据文件中
bool ChatTaskManager::saveBotPersona()
{
    json data;
    // 上读锁
    std::shared_lock<std::shared_mutex> rlock(persona_map_global_lock);
    data["persona_map"] = persona_map;
    return FileManager::writeJsonFile(DATA_PATH + "/AiModule/persona_map.json", data);
}
// 从数据文件中加载 bot 人格
bool ChatTaskManager::loadBotPersona()
{
    json data;
    if(FileManager::readJsonFile(DATA_PATH + "/AiModule/persona_map.json", data))
    {
        if(data.empty())
        {
            Logger::warn("从数据文件中加载bot人格时文件为空: ", DATA_PATH + "/AiModule/persona_map.json");
            return true;
        }
        if(!data.contains("persona_map"))
        {
            Logger::warn("从数据文件中加载bot人格时文件异常: ", DATA_PATH + "/AiModule/persona_map.json");
            return false;
        }
        std::lock_guard<std::shared_mutex> lock(persona_map_global_lock);
        persona_map.clear();
        persona_map = data["persona_map"].get<std::unordered_map<std::string, std::string>>();
        return true;
    }
    return false;
}


// 获取或创建群聊用户画像锁
std::shared_ptr<std::mutex> ChatTaskManager::getUsersProMutex(const std::string& group_id)
{
    std::lock_guard<std::mutex> lock(users_profile_mutex_map_lock);
    if(users_profile_mutex_map.find(group_id) == users_profile_mutex_map.end())
    {
        users_profile_mutex_map[group_id] = std::make_shared<std::mutex>();
    }
    return users_profile_mutex_map[group_id];
}
// 获取或创建群聊用户画像
std::unordered_map<std::string, ChatTaskManager::UserProfile> ChatTaskManager::getUsersProfile(const std::string& group_id, const std::string& user_id)
{
    // 先上一个全局写锁
    std::unique_lock<std::shared_mutex> wlock(users_profile_map_global_lock);
    if(users_profile_map.count(group_id))
    {
        wlock.unlock();
    }
    // 上锁
    std::shared_ptr<std::mutex> users_profile_mutex = getUsersProMutex(group_id);
    std::lock_guard<std::mutex> users_profile_lock(*users_profile_mutex);
    if(users_profile_map[group_id].count(user_id) == 0)
    {
        // 第一次见到该用户, 同时更新昵称和群昵称
        httplib::Client cli(SERVER_HOST, SERVER_PORT);
        httplib::Headers headers = {
            {"Authorization", "Bearer " + SERVER_TOKEN}
        };
        json body;
        body["group_id"] = group_id;
        body["user_id"] = user_id;
        auto res = cli.Post("/get_group_member_info", headers, body.dump(), "application/json");
        if(!res)
        {
            Logger::error("获取群聊成员信息请求失败", httplib::to_string(res.error()));
            return {};
        }
        if(res->status != 200)
        {
            Logger::warn("获取群聊成员信息请求 HTTP状态码: ", res->status);
            Logger::error("获取群聊成员信息请求 异常响应体:", json::parse(res->body).dump(4));
            return {};
        }
        json raw_data = json::parse(res->body);
        // 更新画像
        users_profile_map[group_id][user_id].card = raw_data["data"]["card"];
        users_profile_map[group_id][user_id].nickname = raw_data["data"]["nickname"];
    }
    return users_profile_map[group_id];
}
// 更新群聊用户画像
bool ChatTaskManager::UpdateUserProfile(const std::string& session_id, const std::string& user_id, const std::string& group_id)
{
    // 让 AI 总结用户画像
    httplib::SSLClient cli(AI_HOST, AI_PORT);
    json body;
    httplib::Headers headers = {
        {"Authorization", "Bearer " + AI_KEY},
        {"Content-Type", "application/json"}
    };
    json messages = json::array();
    messages.push_back({
        {"role", "system"},
        {"content", "接下来输入一系列user和assistant的对话历史, 请你返回一句话概括user特点"}
    });
    // 获取对话历史
    std::vector<SessionMemCtx> history = getSessionHistory(session_id);
    // 输入对话历史
    for(auto& msg : history)
    {
        messages.push_back({
            {"role", msg.role},
            {"content", msg.content}
        });
    }
    // 构造请求体
    body["model"] = AI_MODEL;
    body["messages"] = messages;
    body["max_tokens"] = AI_MAX_TOKENS;
    auto res = cli.Post(AI_POST_PATH, headers, body.dump(), "application/json");
    if(!res)
    {
        Logger::error("AI网络请求失败", httplib::to_string(res.error()));
        return false;
    }
    if(res->status != 200)
    {
        Logger::warn("AI请求 HTTP状态码: ", res->status);
        Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
        return false;
    }
    json raw_data = json::parse(res->body);
    if (!raw_data.contains("choices") || raw_data["choices"].empty())
    {
        Logger::warn("AI回复异常", raw_data.dump(4));
        return false;
    }
    std::string des = raw_data["choices"][0]["message"]["content"].get<std::string>();
    // 同时更新昵称和群昵称
    httplib::Client cli2(SERVER_HOST, SERVER_PORT);
    httplib::Headers headers2 = {
        {"Authorization", "Bearer " + SERVER_TOKEN}
    };
    json body2;
    body2["group_id"] = group_id;
    body2["user_id"] = user_id;
    auto res2 = cli2.Post("/get_group_member_info", headers2, body2.dump(), "application/json");
    if(!res2)
    {
        Logger::error("获取群聊成员信息请求失败", httplib::to_string(res2.error()));
        return false;
    }
    if(res2->status != 200)
    {
        Logger::warn("获取群聊成员信息请求 HTTP状态码: ", res2->status);
        Logger::error("获取群聊成员信息请求 异常响应体:", json::parse(res2->body).dump(4));
        return false;
    }
    json raw_data2 = json::parse(res2->body);
    // 上锁
    std::shared_ptr<std::mutex> users_profile_mutex = getUsersProMutex(group_id);
    std::lock_guard<std::mutex> users_profile_lock(*users_profile_mutex);
    // 更新画像
    users_profile_map[group_id][user_id].description = des;
    users_profile_map[group_id][user_id].card = raw_data2["data"]["card"];
    users_profile_map[group_id][user_id].nickname = raw_data2["data"]["nickname"];
    return true;
}
// 保存用户画像到数据文件中
bool ChatTaskManager::saveUsersProfile()
{
    json data;
    // 上读锁
    std::shared_lock<std::shared_mutex> rlock(users_profile_map_global_lock);
    for(auto& [group_id, users] : users_profile_map)
    {
        for(auto& [user_id, user] : users)
        {
            data["users_profile_map"][group_id][user_id] = {
                {"nickname", user.nickname},
                {"card", user.card},
                {"description", user.description}
            };
        }
    }
    return FileManager::writeJsonFile(DATA_PATH + "/AiModule/users_profile_map.json", data);
}
// 从数据文件中加载用户画像
bool ChatTaskManager::loadUserProfile()
{
    json data;
    if(FileManager::readJsonFile(DATA_PATH + "/AiModule/users_profile_map.json", data))
    {
        if(data.empty())
        {
            Logger::warn("从数据文件中加载用户画像时文件为空: ", DATA_PATH + "/AiModule/users_profile_map.json");
            return true;
        }
        if(!data.contains("users_profile_map"))
        {
            Logger::warn("从数据文件中加载用户画像时文件异常: ", DATA_PATH + "/AiModule/users_profile_map.json");
            return false;
        }
        std::lock_guard<std::shared_mutex> lock(users_profile_map_global_lock);
        users_profile_map.clear();
        for(auto& [group_id, users] : data["users_profile_map"].items())
        {
            for(auto& [user_id, user] : users.items())
            {
                UserProfile p;
                p.nickname = user["nickname"];
                p.card = user["card"];
                p.description = user["description"];
                users_profile_map[group_id][user_id] = p;
            }
        }
        return true;
    }
    return false;
}


//与chatAI交互
std::string ChatTaskManager::ChatWithAI(const MessageContext& msgctx)
{
    const std::string& user_input = msgctx.pmsgsegs.text;
    const std::string session_id = getSessionId(msgctx);
    httplib::SSLClient cli(AI_HOST, AI_PORT);
    json body;
    httplib::Headers headers = {
        {"Authorization", "Bearer " + AI_KEY},
        {"Content-Type", "application/json"}
    };
    // 获取或者创建对话历史
    std::vector<SessionMemCtx> history = getSessionHistory(session_id);
    Logger::info("获取对应 session 对话历史成功, 轮数: ", history.size()/2);
    // 获取或者创建bot人格
    std::string persona = getBotPersona(msgctx.group_id);
    Logger::info("获取bot人格成功", "");
    // 获取或创建群聊用户画像
    std::unordered_map<std::string, UserProfile> users = getUsersProfile(msgctx.group_id, msgctx.user_id);
    Logger::info("获取群聊用户画像成功, 画像人数: ", users.size());
    json messages = json::array();
    // 加入系统提示词
    messages.push_back({
        {"role", "system"},
        {"content", "<1>" + AI_SYS_PROMPTS}
    });
    // 加入bot人格
    messages.push_back({
        {"role", "system"},
        {"content", "<2>接下来输入你的<群聊习得人格>"}
    });
    messages.push_back({
        {"role","system"},
        {"content", persona}
    });
    // 加入群聊用户画像
    messages.push_back({
        {"role", "system"},
        {"content", "<3>接下来输入一系列用户画像, 格式为 [id:xxx]_[nickname:xxx]_[des:xxx] , 三个[]内xxx分别对应用户id、昵称和描述"}
    });
    for(auto& user : users)
    {
        messages.push_back({
            {"role", "system"},
            {"content", "[id:" + user.first + "]_[nickname:" + user.second.nickname + "]_[des:" + user.second.description + "]"}
        });
    }
    // 加入对话历史
    messages.push_back({
        {"role", "system"},
        {"content", "<4>接下来输入你和用户id: " + msgctx.user_id + "的对话历史"}
    });
    for(auto& msg : history)
    {
        messages.push_back({
            {"role", msg.role},
            {"content", msg.content}
        });
    }
    // 加入用户输入
    messages.push_back({
        {"role", "system"},
        {"content", "<5>接下来输入用户id: " + msgctx.user_id + "的新提问内容"} 
    });
    messages.push_back({
        {"role", "user"},
        {"content", user_input}
    });
    // 构造请求体
    body["model"] = AI_MODEL;
    body["messages"] = messages;
    body["max_tokens"] = AI_MAX_TOKENS;
    auto res = cli.Post(AI_POST_PATH, headers, body.dump(), "application/json");
    // 硅基流动 post 请求示例
    //         curl --request POST \
    //   --url https://api.siliconflow.cn/v1/chat/completions \
    //   -H "Content-Type: application/json" \
    //   -H "Authorization: Bearer YOUR_API_KEY" \
    //   -d '{
    //     "model": "Pro/zai-org/GLM-4.7",
    //     "messages": [
    //       {"role": "system", "content": "你是一个有用的助手"},
    //       {"role": "user", "content": "你好，请介绍一下你自己"}
    //     ]
    //   }'
    if(!res)
    {
        Logger::error("AI网络请求失败", httplib::to_string(res.error()));
        return "AI网络请求失败";
    }
    if(res->status != 200)
    {
        Logger::warn("AI请求 HTTP状态码: ", res->status);
        Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
        return "AI请求异常";
    }
    json raw_data = json::parse(res->body);
    if (!raw_data.contains("choices") || raw_data["choices"].empty())
    {
        Logger::warn("AI回复异常", raw_data.dump(4));
        return "AI 回复异常";
    }
    std::string ai_reply = raw_data["choices"][0]["message"]["content"].get<std::string>();
    Logger::info("获取ai回复成功, 长度: ", ai_reply.length()); 
    if(UpdateSessionMemory(session_id, user_input, ai_reply))
    {
        // 上锁，更新轮数
        std::lock_guard<std::shared_mutex> lock(session_round_counter_mutex);
        session_round_counter[session_id]++;
        // 同步轮数
        Logger::info("更新对应 session 对话历史成功, 轮数: ", session_round_counter[session_id]);
        if(session_round_counter[session_id] >= AI_MAX_CHAT_ROUNDS/10)
        {
            if(!UpdateUserProfile(session_id, msgctx.user_id, msgctx.group_id))
            {
                Logger::warn("更新用户画像失败, session_id: ", session_id);
            }else{
                // 重置记录的轮数
                session_round_counter[session_id] = 0;
                Logger::info("用户画像已更新, 对应 session 轮数已重置, session_id: ", session_id);
            }
        }
    }
    std::lock_guard<std::shared_mutex> group_round_lock(group_round_counter_mutex);
    group_round_counter[msgctx.group_id]++;
    Logger::info("当前群聊轮数: ", group_round_counter[msgctx.group_id]);
    if(group_round_counter[msgctx.group_id] >= AI_MAX_CHAT_ROUNDS/10)
    {
        if(!UpdateBotPersona(msgctx.group_id))
        {
            Logger::warn("更新bot群聊人格失败, group_id: ", msgctx.group_id);
        }else{
            group_round_counter[msgctx.group_id] = 0;
            Logger::info("更新bot群聊人格成功, 群聊轮数已重置, group_id: ", msgctx.group_id);
        }
    }
    return ai_reply;
}
// 单纯的与纯净模型交互 (豆包)
std::string ChatTaskManager::askAI(const MessageContext& msgctx)
{
    httplib::SSLClient cli(AI_HOST2, AI_PORT);
    cli.enable_server_certificate_verification(true);
    json body;
    httplib::Headers headers = {
        {"Authorization", "Bearer " + AI_KEY2},
        {"Expect", ""}
    };
    json messages = json::array();
    // messages.push_back({
    //     {"role", "system"},
    //     {"content", "You are a helpful assistant."}
    // });
    json input = json::array();
    input.push_back({
        {"type", "text"},
        {"text", msgctx.pmsgsegs.text}
    });
    if(msgctx.pmsgsegs.has_image)
    {
        input.push_back({
            {"type", "image_url"},
            {"image_url", {
                {"url", msgctx.pmsgsegs.image}
            }}
        });
    }
    messages.push_back({
        {"role", "user"},
        {"content", input}
    });
    // 构造请求体
    body["model"] = AI_MODEL2;
    body["messages"] = messages;
    auto res = cli.Post(AI_POST_PATH2, headers, body.dump(), "application/json");
    if(!res)
    {
        Logger::error("AI网络请求失败", httplib::to_string(res.error()));
        return "AI网络请求失败";
    }
    if(res->status != 200)
    {
        Logger::warn("AI请求 HTTP状态码: ", res->status);
        Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
        return "AI请求异常";
    }
    json raw_data = json::parse(res->body);
    if (!raw_data.contains("choices") || raw_data["choices"].empty())
    {
        Logger::warn("AI回复异常", raw_data.dump(4));
        return "AI 回复异常";
    }
    std::string ai_reply = raw_data["choices"][0]["message"]["content"].get<std::string>();
    Logger::info("获取ai回复成功, 长度: ", ai_reply.length());
    return ai_reply;
}

// 定时自动保存数据
bool ChatTaskManager::autoSaveData()
{
    if(!dirty_bit.load()) return true; // 数据无变化不需要写入文件
    return saveSessionRoundCounter() && saveSessionMemory() &&
            saveGroupRoundCounter() && saveBotPersona() && saveUsersProfile();
}

ChatTaskManager::ChatTaskManager()
{
    loadSessionRoundCounter();
    loadGroupRoundCounter();
    Logger::info("对话数量已加载", "");
    loadSessionMemory();
    Logger::info("会话历史已加载", "");
    loadBotPersona();
    Logger::info("Bot 人格已加载", "");
    loadUserProfile();
    Logger::info("用户画像已加载", "");
    dirty_bit.store(false);
    Logger::info("脏位已载入", "");
    running.store(true);
    timer = std::thread([this]{
        while(running)
        {
            autoSaveData();
            std::this_thread::sleep_for(std::chrono::seconds(SAVE_FREQUENCY));
        }
    });
    Logger::info("计时保存线程已启动", "");
}
bool ChatTaskManager::canHandle(const MessageContext& msgctx)
{
    return msgctx.pmsgsegs.at_me && (msgctx.msg_type == "group");
}    

std::pair<json, std::string> ChatTaskManager::handleTask(const MessageContext& msgctx)
{
    json result = json::array();
    result.emplace_back(MessageManager::buildMsg("at", msgctx.user_id));
    if(msgctx.pmsgsegs.text.empty())
    {
        result.emplace_back(MessageManager::buildMsg("text", "请输入对话内容, 以/起始的对话内容由无上下文AI直接处理, 输入 帮助 可查看命令列表"));
        return std::make_pair(result, "direct");
    }
    if(msgctx.pmsgsegs.text[0] == '/')
    {
        result.emplace_back(MessageManager::buildMsg("text", askAI(msgctx)));
    }else{
        result.emplace_back(MessageManager::buildMsg("text", ChatWithAI(msgctx)));
    }
    dirty_bit.store(true, std::memory_order_relaxed);
    return std::make_pair(result, "direct");
}

ChatTaskManager::~ChatTaskManager()
{
    // 停止计时线程
    running.store(false);
    if(timer.joinable()) timer.join();
}

