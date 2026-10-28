#include "BotServer.h"
#include "config.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

static void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

static size_t occurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos)
    {
        ++count;
        position += needle.size();
    }
    return count;
}

static json readJson(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return json::parse(input);
}

static void writeJson(const std::filesystem::path& path, const json& value)
{
    std::ofstream output(path);
    output << value.dump();
    require(output.good(), "无法准备测试数据: " + path.string());
}

template<class Predicate>
static void eventually(Predicate predicate, const std::string& message)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        try { if (predicate()) return; } catch (const json::exception&) {}
        std::this_thread::sleep_for(25ms);
    }
    require(false, message);
}

class Services {
    httplib::Server server;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<json> judge_requests;
    std::vector<json> chat_requests;
    std::vector<json> sent_requests;
    std::string judge_result = "NO";
    std::string chat_result = "CHAT_DEFAULT";
    std::string persona_result = "UPDATED_PERSONA_MARKER";
    std::string profile_result = "UPDATED_PROFILE_MARKER";
    bool summary_invalid = false;
    bool summary_empty = false;
    bool judge_invalid = false;
    bool chat_invalid = false;
    bool send_failure = false;
    bool block_chat = false;
    bool chat_entered = false;
    bool release_chat = false;
    int judge_delay_ms = 0;
    int next_id = 5000;

public:
    int port;

    Services()
    {
        server.Post("/judge", [this](const httplib::Request& request, httplib::Response& response) {
            std::string result;
            bool invalid;
            int delay;
            {
                std::lock_guard<std::mutex> lock(mutex);
                judge_requests.push_back(json::parse(request.body));
                result = judge_result;
                invalid = judge_invalid;
                delay = judge_delay_ms;
            }
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            if (invalid) response.set_content("not-json", "text/plain");
            else response.set_content(json{{"choices", json::array({{{"message", {{"content", result}}}}})}}.dump(), "application/json");
        });
        server.Post("/chat", [this](const httplib::Request& request, httplib::Response& response) {
            std::unique_lock<std::mutex> lock(mutex);
            chat_requests.push_back(json::parse(request.body));
            if (block_chat)
            {
                chat_entered = true;
                condition.notify_all();
                condition.wait(lock, [this] { return release_chat; });
                block_chat = false;
            }
            const std::string instructions = chat_requests.back().at("messages").at(0).at("content").get<std::string>();
            const bool persona = instructions.find("群人格总结器") != std::string::npos;
            const bool profile = instructions.find("用户画像总结器") != std::string::npos;
            const std::string result = (persona || profile) && summary_empty ? "" : persona ? persona_result : profile ? profile_result : chat_result;
            const bool invalid = persona || profile ? summary_invalid : chat_invalid;
            lock.unlock();
            if (invalid) response.set_content("not-json", "text/plain");
            else response.set_content(json{{"choices", json::array({{{"message", {{"content", result}}}}})}}.dump(), "application/json");
        });
        const auto send = [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::mutex> lock(mutex);
            sent_requests.push_back(json::parse(request.body));
            json result = {{"status", send_failure ? "failed" : "ok"}, {"retcode", send_failure ? 100 : 0}, {"data", {{"message_id", ++next_id}}}};
            response.set_content(result.dump(), "application/json");
        };
        server.Post("/send_group_msg", send);
        server.Post("/send_forward_msg", send);
        port = server.bind_to_any_port("127.0.0.1");
        require(port > 0, "本地模拟服务无法绑定端口");
        worker = std::thread([this] { server.listen_after_bind(); });
        eventually([this] { return server.is_running(); }, "本地模拟服务未启动");
    }

    ~Services()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_chat = true;
            condition.notify_all();
        }
        server.stop();
        if (worker.joinable()) worker.join();
    }

    void judge(const std::string& result, bool invalid = false, int delay = 0)
    {
        std::lock_guard<std::mutex> lock(mutex);
        judge_result = result;
        judge_invalid = invalid;
        judge_delay_ms = delay;
    }

    void chat(const std::string& result, bool invalid = false)
    {
        std::lock_guard<std::mutex> lock(mutex);
        chat_result = result;
        chat_invalid = invalid;
    }

    void failSend(bool fail)
    {
        std::lock_guard<std::mutex> lock(mutex);
        send_failure = fail;
    }

    void summariesInvalid(bool invalid, bool empty = false)
    {
        std::lock_guard<std::mutex> lock(mutex);
        summary_invalid = invalid;
        summary_empty = empty;
    }

    std::vector<json> judges() { std::lock_guard<std::mutex> lock(mutex); return judge_requests; }
    std::vector<json> chats() { std::lock_guard<std::mutex> lock(mutex); return chat_requests; }
    std::vector<json> sends() { std::lock_guard<std::mutex> lock(mutex); return sent_requests; }
    std::string lastMessageId() { std::lock_guard<std::mutex> lock(mutex); return std::to_string(next_id); }

    void blockNextChat()
    {
        std::lock_guard<std::mutex> lock(mutex);
        block_chat = true;
        chat_entered = false;
        release_chat = false;
    }

    void awaitChat()
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, 5s, [this] { return chat_entered; }), "聊天请求未进入可控阻塞点");
    }

    void unblockChat()
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_chat = true;
        condition.notify_all();
    }
};

static json text(const std::string& value)
{
    return {{"type", "text"}, {"data", {{"text", value}}}};
}

static json at(const json& qq)
{
    return {{"type", "at"}, {"data", {{"qq", qq}}}};
}

static json reply(const std::string& id)
{
    return {{"type", "reply"}, {"data", {{"id", id}}}};
}

static json event(const json& segments, int group = 123, int user = 77)
{
    static std::atomic<int> id{100};
    return {{"post_type", "message"}, {"message_type", "group"}, {"group_id", group}, {"user_id", user}, {"message_id", ++id}, {"time", 1700000000}, {"sender", {{"user_id", user}, {"nickname", "member_" + std::to_string(user)}, {"card", "card_" + std::to_string(user)}}}, {"message", segments}};
}

static void deliver(BotServer& bot, const json& value)
{
    httplib::Request request;
    httplib::Response response;
    response.status = 200;
    request.body = value.dump();
    bot.handlePost(request, response);
    require(response.status == 200, "合法事件未返回200");
}

static void configure(const Services& services, const std::filesystem::path& root)
{
    LOG_FILE_PATH = (root / "test.log").string();
    LOG_FILE_LEVEL = 3;
    LOG_CONSOLE_LEVEL = 3;
    BOT_QQ = "42";
    BOT_NAME = "TestBot";
    SERVER_HOST = "127.0.0.1";
    SERVER_PORT = services.port;
    SERVER_TOKEN = "test";
    CLIENT_HOST = "127.0.0.1";
    CLIENT_PORT = 0;
    CACHE_PATH = (root / "cache").string();
    DATA_PATH = (root / "data").string();
    SAVE_FREQUENCY = 1;
    AI_HOST = "http://127.0.0.1:" + std::to_string(services.port);
    AI_PORT = services.port;
    AI_POST_PATH = "/chat";
    AI_MODEL = "chat-test";
    AI_KEY = "chat-key";
    AI_SYS_PROMPTS = "SYSTEM_PERSONA_MARKER";
    AI_MAX_TOKENS = 64;
    AI_MAX_CHAT_ROUNDS = 100000;
    AI_MAX_MESSAGES = 120;
    AI_CONTEXT_BYTES = 64000;
    AI_TIMEOUT_SECONDS = 5;
    JUDGE_HOST = AI_HOST;
    JUDGE_PORT = services.port;
    JUDGE_POST_PATH = "/judge";
    JUDGE_MODEL = "judge-test";
    JUDGE_KEY = "judge-key";
    JUDGE_MAX_TOKENS = 16;
    JUDGE_TIMEOUT_SECONDS = 1;
    JUDGE_MAX_MESSAGES = 12;
    JUDGE_CONTEXT_BYTES = 12000;
    CHAT_REPLY_PROBABILITY = 0;
    B23_APPID = "test-b23";
    B23_HOST = "127.0.0.1";
    B23_PORT = 1;
    B23_GET_QUERY_PATH = "/never";
    std::filesystem::create_directories(root / "data/AiModule");
}

static void configuration(const std::filesystem::path& root)
{
    json settings = readJson("config.example.json");
    require(!settings.contains("ai2"), "示例配置仍要求第二聊天模型");
    require(settings.at("ai").at("reply_probability") == 0.1, "示例插话概率未设为10%");
    settings["ai"]["reply_probability"] = 0.37;
    const auto path = root / "config.json";
    writeJson(path, settings);
    load_config(path.string());
    require(CHAT_REPLY_PROBABILITY == 0.37, "插话概率配置未生效");
    settings["ai"]["reply_probability"] = 1.01;
    writeJson(path, settings);
    bool rejected = false;
    try { load_config(path.string()); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "超出范围的插话概率未拒绝");
}

static void routingAndHistory(Services& services)
{
    BotServer bot;
    size_t judges = services.judges().size();
    size_t chats = services.chats().size();
    size_t sends = services.sends().size();
    deliver(bot, event(json::array({at("42"), text(" 时间")})));
    require(services.sends().size() == sends + 1, "@时间未由命令回复");
    require(services.judges().size() == judges && services.chats().size() == chats, "命令被AI抢占");

    const json card = {{"meta", {{"detail_1", {{"appid", B23_APPID}, {"qqdocurl", "invalid-short-link"}}}}}};
    auto card_event = event(json::array({at("42"), text("CARD_PRIORITY_MARKER"), {{"type", "json"}, {"data", {{"data", card.dump()}}}}}));
    JsonTaskManager cards;
    require(cards.canHandle(MessageManager::getMessageContext(card_event)), "符合appid的卡片未由专用处理器匹配");
    json other_card = card;
    other_card["meta"]["detail_1"]["appid"] = "other-app";
    card_event = event(json::array({text("OTHER_CARD_CONTEXT_MARKER"), {{"type", "json"}, {"data", {{"data", other_card.dump()}}}}}));
    require(!cards.canHandle(MessageManager::getMessageContext(card_event)), "其他应用卡片误匹配B站处理器");
    deliver(bot, card_event);
    require(services.judges().size() == ++judges && services.chats().size() == chats, "其他应用卡片的独立文本未按普通文本判定");

    sends = services.sends().size();
    deliver(bot, event(json::array({at("42")})));
    require(services.sends().size() == sends + 1, "仅@未返回输入提示");
    require(services.judges().size() == judges && services.chats().size() == chats, "仅@调用模型");
    deliver(bot, event(json::array({{{"type", "image"}, {"data", {{"url", "IMAGE_ONLY_MARKER"}}}}})));
    deliver(bot, event(json::array({{{"type", "record"}, {"data", {{"file", "VOICE_ONLY_MARKER"}}}}})));
    require(services.sends().size() == sends + 1 && services.judges().size() == judges, "纯媒体触发回复或判定");

    services.chat("DIRECT_REPLY_MARKER");
    deliver(bot, event(json::array({at(42), text("CURRENT_DIRECT_MARKER")})));
    require(services.judges().size() == judges && services.chats().size() == ++chats, "数值@本机器人未直接聊天");
    const std::string sent_id = services.lastMessageId();
    const auto direct = services.chats().back();
    require(occurrences(direct.at("messages").dump(), "CURRENT_DIRECT_MARKER") == 1, "当前输入重复");
    require(direct.at("model") == "chat-test", "使用错误聊天模型");

    deliver(bot, event(json::array({reply(sent_id), text("REPLY_TO_BOT_MARKER")})));
    require(services.chats().size() == ++chats && services.judges().size() == judges, "引用已确认bot回复未直接聊天");
    services.judge("NO");
    for (const auto& segments : {json::array({at("77"), text("AT_OTHER_MARKER")}), json::array({at("all"), text("AT_ALL_MARKER")}), json::array({reply("8001"), text("REPLY_OTHER_MARKER")})})
    {
        deliver(bot, event(segments));
        ++judges;
        require(services.judges().size() == judges && services.chats().size() == chats, "@其他人、全体或引用他人错误直达");
    }

    deliver(bot, event(json::array({text("BOT_ECHO_MARKER")}), 123, 42));
    require(services.judges().size() == judges && services.chats().size() == chats, "机器人自我触发");
    deliver(bot, event(json::array({text("MEMBER_A_SHARED_MARKER")}), 123, 88));
    ++judges;
    deliver(bot, event(json::array({text("OTHER_GROUP_SECRET_MARKER")}), 1234, 99));
    ++judges;
    services.chat("SHARED_REPLY_MARKER");
    deliver(bot, event(json::array({at("42"), text("MEMBER_B_CURRENT_MARKER")}), 123, 77));
    ++chats;
    const std::string shared = services.chats().back().at("messages").dump();
    require(occurrences(shared, "MEMBER_A_SHARED_MARKER") == 1 && occurrences(shared, "MEMBER_B_CURRENT_MARKER") == 1, "群内共享上下文缺失或重复");
    require(shared.find("OTHER_GROUP_SECRET_MARKER") == std::string::npos, "群1234历史混入群123");
    require(shared.find("GROUP_1234_PERSONA_SECRET") == std::string::npos && shared.find("GROUP_1234_PROFILE_SECRET") == std::string::npos, "跨群人格或画像泄漏");
    require(shared.find("GROUP_123_PERSONA_MARKER") != std::string::npos && shared.find("GROUP_123_PROFILE_MARKER") != std::string::npos, "已有群人格或画像未复用");
    require(shared.find("OTHER_CARD_CONTEXT_MARKER") != std::string::npos, "卡片消息未进入群历史");
    require(shared.find("时间") != std::string::npos || shared.find("\\u65f6\\u95f4") != std::string::npos, "命令消息未进入群历史");

    auto cleared_card = event(json::array({at("42"), text("CLEAR_MEMBER_CARD_MARKER")}));
    cleared_card["sender"]["card"] = "";
    deliver(bot, cleared_card);
    ++chats;
    bool cleared = false;
    const auto cleared_request = services.chats().back();
    for (const auto& message : cleared_request.at("messages"))
    {
        const auto background = json::parse(message.at("content").get<std::string>(), nullptr, false);
        if (background.is_object() && background.contains("profiles"))
            for (const auto& profile : background.at("profiles"))
                if (profile.at("user_id") == "77") cleared = profile.at("card") == "";
    }
    require(cleared, "成员清空群名片后画像仍保留旧名片");

    services.judge("YES");
    deliver(bot, event(json::array({text("JUDGE_YES_MARKER")})));
    require(services.judges().size() == ++judges && services.chats().size() == ++chats, "YES未触发主模型");
    const std::string judge = services.judges().back().at("messages").dump();
    require(occurrences(judge, "JUDGE_YES_MARKER") == 1, "判定当前消息重复");
    require(judge.find("DIRECT_REPLY_MARKER") != std::string::npos || judge.find("SHARED_REPLY_MARKER") != std::string::npos, "判定短上下文缺失bot回复");
    require(judge.find("SYSTEM_PERSONA_MARKER") != std::string::npos && judge.find("GROUP_123_PERSONA_MARKER") != std::string::npos, "判定没有复用当前人设");
    require(judge.find("GROUP_123_PROFILE_MARKER") == std::string::npos, "判定携带长期用户画像");

    services.judge("NO");
    sends = services.sends().size();
    deliver(bot, event(json::array({text("NO_SILENT_MARKER")})));
    require(services.judges().size() == ++judges && services.chats().size() == chats && services.sends().size() == sends, "NO且概率0未沉默");
    CHAT_REPLY_PROBABILITY = 1;
    services.chat("[SILENT]");
    deliver(bot, event(json::array({text("PROBABILITY_SILENT_MARKER")})));
    require(services.judges().size() == ++judges && services.chats().size() == ++chats && services.sends().size() == sends, "概率插话沉默标记被发送");
    services.chat("PROBABILITY_REPLY_MARKER");
    deliver(bot, event(json::array({text("PROBABILITY_RESPOND_MARKER")})));
    require(services.chats().size() == ++chats && services.sends().size() == sends + 1, "概率插话未正常发送");
    ++judges;

    for (bool invalid_json : {false, true})
    {
        services.judge("unexpected-label", invalid_json);
        CHAT_REPLY_PROBABILITY = 0;
        deliver(bot, event(json::array({text("INVALID_JUDGE_SILENT")})));
        require(services.judges().size() == ++judges && services.chats().size() == chats, "无效判定被重试或调用聊天兜底");
        CHAT_REPLY_PROBABILITY = 1;
        deliver(bot, event(json::array({text("INVALID_JUDGE_PROBABILITY")})));
        require(services.judges().size() == ++judges && services.chats().size() == ++chats, "无效判定未进入概率路径");
    }

    services.judge("NO", false, 1500);
    CHAT_REPLY_PROBABILITY = 0;
    deliver(bot, event(json::array({text("TIMEOUT_JUDGE_SILENT")})));
    require(services.judges().size() == ++judges && services.chats().size() == chats, "判定超时重试或触发聊天");
    CHAT_REPLY_PROBABILITY = 1;
    deliver(bot, event(json::array({text("TIMEOUT_JUDGE_PROBABILITY")})));
    require(services.judges().size() == ++judges && services.chats().size() == ++chats, "判定超时概率命中未触发聊天");
    services.judge("NO");
    const auto judge_host = JUDGE_HOST;
    JUDGE_HOST = "http://127.0.0.1:1";
    CHAT_REPLY_PROBABILITY = 0;
    deliver(bot, event(json::array({text("NETWORK_JUDGE_SILENT")})));
    require(services.chats().size() == chats, "判定网络失败概率0触发聊天");
    CHAT_REPLY_PROBABILITY = 1;
    deliver(bot, event(json::array({text("NETWORK_JUDGE_PROBABILITY")})));
    require(services.chats().size() == ++chats, "判定网络失败概率1未触发聊天");
    deliver(bot, event(json::array({at("42"), text("DIRECT_WITH_JUDGE_DOWN")})));
    require(services.chats().size() == ++chats && services.judges().size() == judges, "判定不可用阻塞直接唤醒");
    JUDGE_HOST = judge_host;
    CHAT_REPLY_PROBABILITY = 0;

    services.chat("SLASH_REPLY_MARKER");
    deliver(bot, event(json::array({at("42"), text("/SLASH_CURRENT_MARKER")})));
    require(services.chats().size() == ++chats && services.chats().back().at("model") == "chat-test", "/切换了聊天模型");
    require(services.chats().back().at("messages").dump().find("MEMBER_A_SHARED_MARKER") != std::string::npos, "/绕过了共享上下文");

    services.chat("FAILED_DELIVERY_MARKER");
    services.failSend(true);
    deliver(bot, event(json::array({at("42"), text("FAILED_DELIVERY_INPUT")})));
    services.failSend(false);
    services.chat("AFTER_FAILED_DELIVERY");
    deliver(bot, event(json::array({at("42"), text("CHECK_FAILED_DELIVERY")})));
    const std::string after_failure = services.chats().back().at("messages").dump();
    require(after_failure.find("FAILED_DELIVERY_INPUT") != std::string::npos && after_failure.find("FAILED_DELIVERY_MARKER") == std::string::npos, "发送失败生成内容冒充已发送历史");

    services.chat("FAILED_GENERATION_MARKER", true);
    deliver(bot, event(json::array({at("42"), text("FAILED_GENERATION_INPUT")})));
    services.chat("AFTER_FAILED_GENERATION");
    deliver(bot, event(json::array({at("42"), text("CHECK_FAILED_GENERATION")})));
    const std::string generation_failure = services.chats().back().at("messages").dump();
    require(generation_failure.find("FAILED_GENERATION_INPUT") != std::string::npos && generation_failure.find("FAILED_GENERATION_MARKER") == std::string::npos, "生成失败丢失入站或记入假回复");

    const auto before_private_judges = services.judges().size();
    const auto before_private_sends = services.sends().size();
    json private_event = event(json::array({at("42"), text("PRIVATE_MESSAGE_MARKER")}));
    private_event["message_type"] = "private";
    private_event.erase("group_id");
    deliver(bot, private_event);
    deliver(bot, json{{"post_type", "notice"}});
    require(services.judges().size() == before_private_judges && services.sends().size() == before_private_sends, "私聊或非消息事件触发业务回复");
}

static void windowBudgetAndConcurrency(Services& services)
{
    BotServer bot;
    services.judge("NO");
    services.chat("WINDOW_REPLY");
    CHAT_REPLY_PROBABILITY = 0;
    AI_MAX_MESSAGES = 4;
    JUDGE_MAX_MESSAGES = 2;
    for (int i = 0; i < 8; ++i)
        deliver(bot, event(json::array({text("WINDOW_HISTORY_" + std::to_string(i))}), 321));
    deliver(bot, event(json::array({at("42"), text("WINDOW_CURRENT_MARKER")}), 321));
    std::string messages = services.chats().back().at("messages").dump();
    require(messages.find("WINDOW_HISTORY_0") == std::string::npos && messages.find("WINDOW_HISTORY_7") != std::string::npos, "聊天窗口未裁剪最早历史");
    deliver(bot, event(json::array({text("JUDGE_WINDOW_CURRENT")}), 321));
    messages = services.judges().back().at("messages").dump();
    require(messages.find("WINDOW_HISTORY_6") == std::string::npos && occurrences(messages, "JUDGE_WINDOW_CURRENT") == 1, "判定未使用较短同源窗口");

    AI_CONTEXT_BYTES = 1800;
    JUDGE_CONTEXT_BYTES = 1600;
    AI_SYS_PROMPTS = std::string(5000, 'P');
    deliver(bot, event(json::array({at("42"), text("BUDGET_CURRENT_" + std::string(6000, 'X'))}), 321));
    const json chat = services.chats().back();
    require(chat.at("messages").dump().size() + 4 * AI_MAX_TOKENS <= AI_CONTEXT_BYTES, "聊天总预算未包含人设、元数据和输出预留");
    require(chat.at("messages").dump().find("BUDGET_CURRENT_") != std::string::npos, "超预算丢失当前消息");
    deliver(bot, event(json::array({text("JUDGE_BUDGET_CURRENT_" + std::string(6000, 'Y'))}), 321));
    const json judge = services.judges().back();
    require(judge.at("messages").dump().size() + 4 * JUDGE_MAX_TOKENS <= JUDGE_CONTEXT_BYTES, "判定总预算未生效");
    require(judge.at("messages").dump().find("JUDGE_BUDGET_CURRENT_") != std::string::npos, "判定超预算丢失当前消息");

    AI_CONTEXT_BYTES = 64000;
    JUDGE_CONTEXT_BYTES = 12000;
    AI_SYS_PROMPTS = "SYSTEM_PERSONA_MARKER";
    AI_MAX_MESSAGES = 120;
    JUDGE_MAX_MESSAGES = 12;
    services.chat("CONCURRENT_SENT_REPLY");
    services.blockNextChat();
    auto pending = std::async(std::launch::async, [&bot] { deliver(bot, event(json::array({at("42"), text("CONCURRENT_FIRST_INPUT")}), 456)); });
    services.awaitChat();
    auto incoming = std::async(std::launch::async, [&bot] { deliver(bot, event(json::array({{{"type", "image"}, {"data", {{"url", "CONCURRENT_LATER_IMAGE"}}}}, text("")}), 456)); });
    const bool collected_during_generation = incoming.wait_for(1s) == std::future_status::ready;
    services.unblockChat();
    pending.get();
    incoming.get();
    require(collected_during_generation, "生成期间纯媒体入站被聊天请求阻塞");
    deliver(bot, event(json::array({at("42"), text("CONCURRENT_CHECK_INPUT")}), 456));
    messages = services.chats().back().at("messages").dump();
    require(messages.find("CONCURRENT_FIRST_INPUT") != std::string::npos && messages.find("CONCURRENT_SENT_REPLY") != std::string::npos, "并发历史丢失已接收消息或成功回复");
    require(messages.find("image") != std::string::npos, "生成期间新消息未保留媒体标记");
}

static void persistenceWithoutAi(Services& services, const std::filesystem::path& root)
{
    const auto timeline = root / "data/AiModule/group_timeline.json";
    const auto judges = services.judges().size();
    const auto chats = services.chats().size();
    services.judge("NO");
    CHAT_REPLY_PROBABILITY = 0;
    {
        BotServer bot;
        deliver(bot, event(json::array({text("PERSIST_WITHOUT_AI_MARKER")}), 654, 81));
        require(services.judges().size() == judges + 1 && services.chats().size() == chats, "持久化样本意外触发聊天");
        eventually([&] { return std::filesystem::exists(timeline) && readJson(timeline).dump().find("PERSIST_WITHOUT_AI_MARKER") != std::string::npos; }, "未触发AI的普通消息未定时保存");
    }
    {
        BotServer bot;
        services.chat("RESTORED_REPLY");
        deliver(bot, event(json::array({at("42"), text("RESTORE_CHECK_MARKER")}), 654, 82));
        const std::string messages = services.chats().back().at("messages").dump();
        require(messages.find("PERSIST_WITHOUT_AI_MARKER") != std::string::npos, "重启未恢复已经保存的普通群历史");
        require(messages.find("LEGACY_HISTORY_MARKER") == std::string::npos, "旧个人历史被伪造为群时间线");
    }
}

static void summaries(Services& services, const std::filesystem::path& root)
{
    AI_MAX_CHAT_ROUNDS = 10;
    CHAT_REPLY_PROBABILITY = 0;
    services.judge("NO");
    services.chat("SUMMARY_CHAT_REPLY");
    {
        BotServer bot;
        deliver(bot, event(json::array({text("OTHER_MEMBER_SUMMARY_SECRET")}), 777, 8));
        deliver(bot, event(json::array({at("42"), text("PROFILE_OWNER_FIRST_MARKER")}), 777, 7));
        const size_t before = services.chats().size();
        deliver(bot, event(json::array({at("42"), text("PROFILE_OWNER_SECOND_MARKER")}), 777, 7));
        const auto calls = services.chats();
        bool saw_persona = false;
        bool saw_profile = false;
        for (size_t i = before; i < calls.size(); ++i)
        {
            const std::string instructions = calls[i].at("messages").at(0).at("content").get<std::string>();
            const std::string messages = calls[i].at("messages").dump();
            if (instructions.find("群人格总结器") != std::string::npos)
            {
                saw_persona = true;
                require(messages.find("OTHER_MEMBER_SUMMARY_SECRET") != std::string::npos, "群人格总结遗漏本群其他成员发言");
                require(messages.find("OTHER_GROUP_SECRET_MARKER") == std::string::npos, "群人格总结混入其他群发言");
            }
            if (instructions.find("用户画像总结器") != std::string::npos)
            {
                saw_profile = true;
                require(messages.find("PROFILE_OWNER_FIRST_MARKER") != std::string::npos, "成员画像总结遗漏本人发言");
                require(messages.find("OTHER_MEMBER_SUMMARY_SECRET") == std::string::npos, "其他成员发言被归入当前成员画像");
            }
            require(calls[i].at("model") == AI_MODEL, "总结使用了独立判定模型");
            require(calls[i].at("messages").dump().size() + 4 * AI_MAX_TOKENS <= AI_CONTEXT_BYTES, "总结请求超出主模型预算");
        }
        require(saw_persona && saw_profile, "成功出站后的群人格或成员画像总结未触发");
        eventually([&] {
            return readJson(root / "data/AiModule/persona_map.json").dump().find("UPDATED_PERSONA_MARKER") != std::string::npos &&
                readJson(root / "data/AiModule/users_profile_map.json").dump().find("UPDATED_PROFILE_MARKER") != std::string::npos;
        }, "总结结果未保存");
        for (bool invalid : {true, false})
        {
            services.summariesInvalid(invalid, !invalid);
            deliver(bot, event(json::array({at("42"), text("SUMMARY_FAILURE_CHECK")}), 777, 7));
            const std::string messages = services.chats().back().at("messages").dump();
            require(messages.find("UPDATED_PERSONA_MARKER") != std::string::npos && messages.find("UPDATED_PROFILE_MARKER") != std::string::npos, "总结失败或空结果覆盖已有群人格或成员画像");
        }
        services.summariesInvalid(false);
    }
    AI_MAX_CHAT_ROUNDS = 100000;
}

static void legacyEmptyAndDamaged(const std::filesystem::path& root)
{
    const std::string original_data = DATA_PATH;
    DATA_PATH = (root / "empty-data").string();
    const auto data = std::filesystem::path(DATA_PATH) / "AiModule";
    std::filesystem::create_directories(data);
    writeJson(data / "persona_map.json", nullptr);
    writeJson(data / "users_profile_map.json", nullptr);
    writeJson(data / "session_round_counter.json", {{"session_round_counter", json::object()}});
    writeJson(data / "group_round_counter.json", {{"group_round_counter", json::object()}});
    {
        BotServer bot;
        deliver(bot, event(json::array({text("NULL_LEGACY_MESSAGE_MARKER")})));
        eventually([&] { return readJson(data / "group_timeline.json").dump().find("NULL_LEGACY_MESSAGE_MARKER") != std::string::npos; }, "旧null格式文件阻止新消息保存");
    }
    const auto path = data / "persona_map.json";
    {
        std::ofstream output(path);
        output << "{damaged-json";
    }
    bool rejected = false;
    try { BotServer bot; } catch (const std::exception&) { rejected = true; }
    require(rejected, "损坏的持久化JSON未阻止启动");
    std::ifstream input(path);
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(content == "{damaged-json", "损坏的旧数据文件被覆盖");
    DATA_PATH = original_data;
}

int main()
{
    const std::filesystem::path root = "release/tests/runtime";
    bool created = false;
    try
    {
        require(!std::filesystem::exists(root), "测试运行目录已存在，请确认前次运行状态后清理");
        std::filesystem::create_directories(root);
        created = true;
        configuration(root);
        Services services;
        configure(services, root);
        const auto data = root / "data/AiModule";
        const json legacy = {{"session_memory", {{"group_123_user_77", json::array({{{"role", "user"}, {"content", "LEGACY_HISTORY_MARKER"}}})}}}};
        writeJson(data / "session_memory.json", legacy);
        writeJson(data / "persona_map.json", {{"persona_map", {{"123", "GROUP_123_PERSONA_MARKER"}, {"1234", "GROUP_1234_PERSONA_SECRET"}}}});
        writeJson(data / "users_profile_map.json", {{"users_profile_map", {{"123", {{"77", {{"nickname", "LegacyMember"}, {"card", "LegacyCard"}, {"description", "GROUP_123_PROFILE_MARKER"}}}}}, {"1234", {{"99", {{"nickname", "SecretMember"}, {"card", "SecretCard"}, {"description", "GROUP_1234_PROFILE_SECRET"}}}}}}}});
        routingAndHistory(services);
        std::cout << "分流、共享上下文、发送回执通过\n";
        windowBudgetAndConcurrency(services);
        std::cout << "窗口、输入预算、并发收集通过\n";
        persistenceWithoutAi(services, root);
        summaries(services, root);
        legacyEmptyAndDamaged(root);
        require(readJson(data / "session_memory.json") == legacy, "旧个人历史文件被修改");
        std::cout << "普通消息定时保存、重启恢复、旧文件保留通过\n";
        std::cout << "人格与画像总结隔离、失败保留通过\n";
        if (created) std::filesystem::remove_all(root);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "测试失败: " << error.what() << '\n';
        if (created) std::filesystem::remove_all(root);
        return 1;
    }
}
