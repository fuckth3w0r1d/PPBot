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
    std::vector<json> poke_requests;
    std::string judge_result = "NO";
    std::string chat_result = "CHAT_DEFAULT";
    std::string persona_result = "UPDATED_PERSONA_MARKER";
    std::string profile_result = "UPDATED_PROFILE_MARKER";
    bool summary_invalid = false;
    bool summary_empty = false;
    bool judge_invalid = false;
    bool chat_invalid = false;
    bool send_failure = false;
    int chat_status = 200;
    int poke_status = 200;
    json poke_result = {{"status", "ok"}, {"retcode", 0}, {"data", nullptr}};
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
            const int status = persona || profile ? 200 : chat_status;
            lock.unlock();
            response.status = status;
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
        server.Post("/group_poke", [this](const httplib::Request& request, httplib::Response& response) {
            std::lock_guard<std::mutex> lock(mutex);
            poke_requests.push_back(json::parse(request.body));
            response.status = poke_status;
            response.set_content(poke_result.dump(), "application/json");
        });
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

    void chat(const std::string& result, bool invalid = false, int status = 200)
    {
        std::lock_guard<std::mutex> lock(mutex);
        chat_result = result;
        chat_invalid = invalid;
        chat_status = status;
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

    void pokeResponse(int status = 200, const json& result = {{"status", "ok"}, {"retcode", 0}, {"data", nullptr}})
    {
        std::lock_guard<std::mutex> lock(mutex);
        poke_status = status;
        poke_result = result;
    }

    std::vector<json> judges() { std::lock_guard<std::mutex> lock(mutex); return judge_requests; }
    std::vector<json> chats() { std::lock_guard<std::mutex> lock(mutex); return chat_requests; }
    std::vector<json> sends() { std::lock_guard<std::mutex> lock(mutex); return sent_requests; }
    std::vector<json> pokes() { std::lock_guard<std::mutex> lock(mutex); return poke_requests; }
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

static json image(const std::string& url)
{
    return {{"type", "image"}, {"data", {{"url", url}}}};
}

static json modelContent(const json& message)
{
    const auto& content = message.at("content");
    if (content.is_string()) return json::parse(content.get<std::string>(), nullptr, false);
    for (const auto& part : content)
        if (part.at("type") == "text") return json::parse(part.at("text").get<std::string>(), nullptr, false);
    return nullptr;
}

static json imageUrls(const json& messages)
{
    json urls = json::array();
    for (const auto& message : messages)
        if (message.at("content").is_array())
            for (const auto& part : message.at("content"))
                if (part.at("type") == "image_url") urls.push_back(part.at("image_url").at("url"));
    return urls;
}

static void replyForm(const json& sent, const std::string& id = "")
{
    const auto& segments = sent.at("message");
    require(segments.size() == (id.empty() ? 1 : 2), "聊天回复包含额外消息段");
    require(segments.back().at("type") == "text", "聊天回复正文未使用文本段");
    if (!id.empty())
        require(segments.front().at("type") == "reply" && segments.front().at("data").at("id") == id,
            "聊天回复未引用本次触发消息");
}

static json poke(int group = 901, int user = 77, int target = 42)
{
    return {{"post_type", "notice"}, {"notice_type", "notify"}, {"sub_type", "poke"},
        {"group_id", group}, {"user_id", user}, {"target_id", target}, {"time", 1700000001}};
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
    const auto empty_mention = event(json::array({at("42")}));
    deliver(bot, empty_mention);
    require(services.sends().size() == sends + 1, "仅@未返回输入提示");
    replyForm(services.sends().back(), empty_mention.at("message_id").dump());
    require(services.judges().size() == judges && services.chats().size() == chats, "仅@调用模型");
    deliver(bot, event(json::array({{{"type", "image"}, {"data", {{"url", "IMAGE_ONLY_MARKER"}}}}})));
    ++judges;
    deliver(bot, event(json::array({{{"type", "record"}, {"data", {{"file", "VOICE_ONLY_MARKER"}}}}})));
    require(services.sends().size() == sends + 1 && services.judges().size() == judges, "纯图片未经过判定或语音触发回复");

    services.chat("DIRECT_REPLY_MARKER");
    deliver(bot, event(json::array({at(42), text("CURRENT_DIRECT_MARKER")})));
    require(services.judges().size() == judges && services.chats().size() == ++chats, "数值@本机器人未直接聊天");
    const std::string sent_id = services.lastMessageId();
    const auto direct = services.chats().back();
    require(occurrences(direct.at("messages").dump(), "CURRENT_DIRECT_MARKER") == 1, "当前输入重复");
    require(direct.at("model") == "chat-test", "使用错误聊天模型");

    deliver(bot, event(json::array({reply(sent_id), text("REPLY_TO_BOT_MARKER")})));
    replyForm(services.sends().back());
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
        const auto background = modelContent(message);
        if (background.is_object() && background.contains("profiles"))
            for (const auto& profile : background.at("profiles"))
                if (profile.at("user_id") == "77") cleared = profile.at("card") == "";
    }
    require(cleared, "成员清空群名片后画像仍保留旧名片");

    services.judge("YES");
    deliver(bot, event(json::array({text("JUDGE_YES_MARKER")})));
    replyForm(services.sends().back());
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
    replyForm(services.sends().back());
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

static void multimodalAndReplies(Services& services, const std::filesystem::path& root)
{
    BotServer bot;
    services.judge("NO");
    services.chat("IMAGE_DIRECT_REPLY");
    CHAT_REPLY_PROBABILITY = 0;
    size_t judges = services.judges().size();
    size_t chats = services.chats().size();
    const std::string first_url = "https://images.example/first.png";
    const std::string second_url = "http://images.example/second.png";
    auto current = event(json::array({at("42"), image(first_url), image(second_url)}), 810, 88);
    deliver(bot, current);
    require(services.judges().size() == judges && services.chats().size() == ++chats, "@纯图片没有直接聊天");
    const auto direct = services.chats().back().at("messages");
    require(imageUrls(direct) == json::array({first_url, second_url}), "当前多张图片未作为图文输入完整传入");
    const auto source = modelContent(direct.back());
    require(source.at("user_id") == "88" && source.at("message_id") == current.at("message_id").dump(), "当前图片失去来源身份");
    replyForm(services.sends().back(), current.at("message_id").dump());
    const std::string bot_id = services.lastMessageId();

    current = event(json::array({reply(bot_id), image("https://images.example/referenced.png")}), 810);
    deliver(bot, current);
    require(services.chats().size() == ++chats && services.judges().size() == judges, "引用机器人并附纯图片没有直接聊天");
    replyForm(services.sends().back());

    current = event(json::array({at("42"), reply(bot_id), text("MENTION_WITH_OLD_REPLY")}), 810);
    deliver(bot, current);
    ++chats;
    replyForm(services.sends().back(), current.at("message_id").dump());
    for (const auto& invalid_id : {json(), json::object(), json(true), json(""), json("invalid-id")})
    {
        current = event(json::array({at("42"), text("MENTION_WITHOUT_VALID_ID")}), 810);
        current["message_id"] = invalid_id;
        deliver(bot, current);
        ++chats;
        replyForm(services.sends().back());
    }
    current.erase("message_id");
    deliver(bot, current);
    ++chats;
    replyForm(services.sends().back());
    current["message_id"] = -100;
    deliver(bot, current);
    ++chats;
    replyForm(services.sends().back(), "-100");

    services.judge("YES");
    deliver(bot, event(json::array({image("https://images.example/judged.png")}), 811));
    require(services.judges().size() == ++judges && services.chats().size() == ++chats, "普通纯图片判定通过未聊天");
    const auto judge = services.judges().back().at("messages");
    require(imageUrls(judge).empty() && judge.dump().find("https://images.example/") == std::string::npos &&
        judge.dump().find("image") != std::string::npos, "判定收到图片URL或缺少图片类型标记");
    require(imageUrls(services.chats().back().at("messages")) == json::array({"https://images.example/judged.png"}), "判定通过未向主模型传图");
    replyForm(services.sends().back());
    services.judge("NO");
    size_t sends = services.sends().size();
    deliver(bot, event(json::array({image("https://images.example/silent.png")}), 812));
    require(services.judges().size() == ++judges && services.chats().size() == chats && services.sends().size() == sends,
        "图片概率未命中仍请求主模型或发送提示");
    CHAT_REPLY_PROBABILITY = 1;
    services.chat("[SILENT]");
    deliver(bot, event(json::array({image("https://images.example/probability-silent.png")}), 813));
    require(services.judges().size() == ++judges && services.chats().size() == ++chats && services.sends().size() == sends,
        "图片概率插话未允许模型沉默");
    services.chat("IMAGE_PROBABILITY_REPLY");
    deliver(bot, event(json::array({image("https://images.example/probability-reply.png")}), 814));
    require(services.judges().size() == ++judges && services.chats().size() == ++chats && services.sends().size() == sends + 1,
        "图片概率插话未传入主模型或未回复");
    require(imageUrls(services.chats().back().at("messages")) == json::array({"https://images.example/probability-reply.png"}), "图片概率分支丢失图片");
    replyForm(services.sends().back());
    CHAT_REPLY_PROBABILITY = 0;

    const std::string historical_url = "https://images.example/member-a.png";
    const auto historical = event(json::array({image(historical_url)}), 815, 88);
    deliver(bot, historical);
    deliver(bot, event(json::array({image("https://images.example/other-group.png")}), 816, 99));
    current = event(json::array({at("42"), text("ASK_MEMBER_A_IMAGE")}), 815, 77);
    deliver(bot, current);
    const auto history = services.chats().back().at("messages");
    require(imageUrls(history) == json::array({historical_url}), "历史图片丢失、重复或混入其他群图片");
    require(occurrences(history.dump(), "ASK_MEMBER_A_IMAGE") == 1, "当前图片追问进入请求多次");
    bool attributed = false;
    for (const auto& message : history)
    {
        const auto record = modelContent(message);
        if (record.is_object() && record.value("message_id", "") == historical.at("message_id").dump())
            attributed = record.at("user_id") == "88" && imageUrls(json::array({message})) == json::array({historical_url});
    }
    require(attributed, "历史图片未与原发言人及消息对应");
    AI_MAX_MESSAGES = 1;
    deliver(bot, event(json::array({at("42"), text("CROPPED_IMAGE_QUERY")}), 815));
    require(imageUrls(services.chats().back().at("messages")).empty(), "裁剪历史消息后仍发送所属图片");
    AI_MAX_MESSAGES = 120;

    current = event(json::array({at("42"), {{"type", "image"}, {"data", json::object()}}}), 817);
    deliver(bot, current);
    const auto unreadable = services.chats().back().at("messages");
    bool unavailable_image = false;
    const auto unreadable_record = modelContent(unreadable.back());
    for (const auto& media : unreadable_record.at("media"))
        if (media.at("type") == "image" && media.contains("unavailable") && media.at("unavailable").is_string())
            unavailable_image = !media.at("unavailable").get<std::string>().empty();
    require(imageUrls(unreadable).empty() && unavailable_image, "缺URL图片没有保留不可读取标记");
    current = event(json::array({at("42"), image("file:///unreadable.png")}), 818);
    deliver(bot, current);
    require(imageUrls(services.chats().back().at("messages")).empty(), "非HTTP(S)地址被作为图片输入");

    services.chat("MODEL_REJECTED_IMAGE", false, 400);
    sends = services.sends().size();
    chats = services.chats().size();
    current = event(json::array({at("42"), image("https://images.example/expired.png")}), 819);
    deliver(bot, current);
    require(services.chats().size() == chats + 1 && services.sends().size() == sends + 1, "读图失败未提示或自动重试");
    replyForm(services.sends().back(), current.at("message_id").dump());
    require(services.sends().back().dump().find("MODEL_REJECTED_IMAGE") == std::string::npos, "读图失败伪造成功正文");
    services.judge("YES");
    deliver(bot, event(json::array({image("https://images.example/ordinary-rejected.png")}), 820));
    require(services.chats().size() == chats + 2 && services.sends().size() == sends + 1, "普通读图失败伪造成功回复或自动重试");
    services.judge("NO");
    services.chat("AFTER_IMAGE_FAILURE");

    const auto original_budget = AI_CONTEXT_BYTES;
    AI_CONTEXT_BYTES = 6000;
    deliver(bot, event(json::array({image("https://images.example/" + std::string(10000, 'U'))}), 821));
    deliver(bot, event(json::array({at("42"), text("URL_BUDGET_CURRENT")}), 821));
    const auto bounded = services.chats().back().at("messages");
    require(bounded.dump().size() + 4 * AI_MAX_TOKENS <= AI_CONTEXT_BYTES && imageUrls(bounded).empty(), "图片URL未纳入字节预算裁剪");
    require(bounded.dump().find("URL_BUDGET_CURRENT") != std::string::npos, "图片URL预算裁剪丢失当前问题");
    chats = services.chats().size();
    sends = services.sends().size();
    current = event(json::array({at("42"), image("https://images.example/" + std::string(10000, 'V'))}), 823);
    deliver(bot, current);
    require(services.chats().size() == chats && services.sends().size() == sends + 1, "当前图片超过预算仍发请求或未提示失败");
    replyForm(services.sends().back(), current.at("message_id").dump());
    require(services.sends().back().dump().find("AFTER_IMAGE_FAILURE") == std::string::npos, "图片预算不足伪造聊天成功");
    AI_CONTEXT_BYTES = original_budget;

    const auto image_trigger = MessageManager::getMessageContext(event(json::array({text("BOT_IMAGE_COMMAND")}), 824));
    const auto image_receipts = MessageManager::send_msg(image_trigger,
        json::array({MessageManager::buildMsg("image", "https://images.example/sent-file.png")}), "direct");
    require(image_receipts.size() == 1 && !image_receipts.front().message_id.empty(), "机器人图片未获得发送确认");
    bot.tsk_manager.recordOutgoing(image_trigger, image_receipts.front());
    auto image_echo = event(json::array({image("https://images.example/bot-history.png")}), 824, 42);
    image_echo["message_id"] = image_receipts.front().message_id;
    deliver(bot, image_echo);
    image_echo["message"] = json::array({MessageManager::buildMsg("image", "https://images.example/sent-file.png")});
    deliver(bot, image_echo);
    deliver(bot, event(json::array({at("42"), text("BOT_IMAGE_HISTORY_QUERY")}), 824));
    bool bot_image = false;
    const auto bot_history = services.chats().back().at("messages");
    for (const auto& message : bot_history)
    {
        const auto record = modelContent(message);
        if (record.is_object() && record.value("user_id", "") == "42" && !imageUrls(json::array({message})).empty())
            bot_image = message.at("role") == "user" && record.at("bot") == true &&
                imageUrls(json::array({message})) == json::array({"https://images.example/bot-history.png"});
    }
    require(bot_image, "机器人历史图片未使用可传图的user角色或丢失bot身份");
    require(imageUrls(bot_history) == json::array({"https://images.example/bot-history.png"}), "同ID自身回流丢失或重复图片URL");
    eventually([&] {
        const auto timeline = readJson(root / "data/AiModule/group_timeline.json").at("group_timeline");
        if (!timeline.contains("824")) return false;
        size_t copies = 0;
        bool retained_url = false;
        for (const auto& record : timeline.at("824"))
            if (record.at("message_id") == image_receipts.front().message_id)
            {
                ++copies;
                retained_url = record.at("segments").dump().find("https://images.example/bot-history.png") != std::string::npos;
            }
        return copies == 1 && retained_url;
    }, "同ID图片回流未补充持久化URL、重复记录或被缺URL回流擦除");

    services.blockNextChat();
    auto pending = std::async(std::launch::async, [&bot] { deliver(bot, event(json::array({at("42"), text("IMAGE_CONCURRENT_FIRST")}), 822)); });
    services.awaitChat();
    auto incoming = std::async(std::launch::async, [&bot] { deliver(bot, event(json::array({image("https://images.example/concurrent.png")}), 822, 88)); });
    bool collected = false;
    try
    {
        eventually([&] {
            return std::filesystem::exists(root / "data/AiModule/group_timeline.json") &&
                readJson(root / "data/AiModule/group_timeline.json").dump().find("https://images.example/concurrent.png") != std::string::npos;
        }, "图片未在生成期间进入群时间线");
        collected = true;
    }
    catch (const std::runtime_error&) {}
    services.unblockChat();
    pending.get();
    incoming.get();
    require(collected, "图片未在生成期间进入群时间线");
    deliver(bot, event(json::array({at("42"), text("IMAGE_CONCURRENT_CHECK")}), 822));
    require(imageUrls(services.chats().back().at("messages")) == json::array({"https://images.example/concurrent.png"}), "生成完成覆盖期间收到的图片");
}

static void pokeActions(Services& services, const std::filesystem::path& root)
{
    BotServer bot;
    CHAT_REPLY_PROBABILITY = 0;
    services.judge("NO");
    const auto judges = services.judges().size();
    size_t chats = services.chats().size();
    size_t sends = services.sends().size();
    size_t pokes = services.pokes().size();

    services.chat(json{{"text", "POKE_TEXT_ONLY"}, {"poke", false}}.dump());
    deliver(bot, poke());
    require(services.judges().size() == judges && services.chats().size() == ++chats &&
        services.sends().size() == ++sends && services.pokes().size() == pokes, "拍机器人未直接选择文字回应");
    replyForm(services.sends().back());
    const auto incoming = modelContent(services.chats().back().at("messages").back());
    require(incoming.at("event_type") == "poke" && incoming.at("target_id") == "42" && incoming.at("user_id") == "77" &&
        incoming.value("message_id", "").empty(), "实际拍事件未以独立行为及正确身份进入模型上下文");

    services.chat(json{{"text", ""}, {"poke", true}}.dump());
    deliver(bot, poke(902));
    require(services.chats().size() == ++chats && services.sends().size() == sends && services.pokes().size() == ++pokes,
        "纯回拍未作为有效回应或发送了空文本");
    require(services.pokes().back() == json{{"group_id", 902}, {"user_id", 77}}, "回拍接口不是本次群号和发起者");

    services.chat(json{{"text", "POKE_BOTH_TEXT mentions user 999"}, {"poke", true}, {"group_id", 999}, {"user_id", 999}}.dump());
    deliver(bot, poke(903, 88));
    require(services.chats().size() == ++chats && services.sends().size() == ++sends && services.pokes().size() == ++pokes,
        "文字与动作未分别执行");
    replyForm(services.sends().back());
    require(services.pokes().back() == json{{"group_id", 903}, {"user_id", 88}}, "模型字段或正文改变了回拍群号或对象");

    std::vector<json> ignored = {poke(904, 77, 99), poke(904, 42), poke(904, 42, 99)};
    auto private_poke = poke();
    private_poke.erase("group_id");
    ignored.push_back(private_poke);
    auto other_notice = poke();
    other_notice["sub_type"] = "lucky_king";
    ignored.push_back(other_notice);
    other_notice = poke();
    other_notice["notice_type"] = "group_increase";
    ignored.push_back(other_notice);
    for (const auto& value : ignored) deliver(bot, value);
    require(services.chats().size() == chats && services.judges().size() == judges && services.sends().size() == sends &&
        services.pokes().size() == pokes, "拍其他成员、自身、私聊或其他通知触发业务回复");

    const auto plain_decision = json{{"text", "拍一拍 user 99"}, {"poke", true}}.dump();
    services.chat(plain_decision);
    deliver(bot, event(json::array({at("42"), text("请拍一拍其他群成员")}), 904));
    require(services.chats().size() == ++chats && services.sends().size() == ++sends && services.pokes().size() == pokes,
        "普通聊天正文被当成回拍指令");
    require(services.sends().back().at("message").back().at("data").at("text") == plain_decision, "普通聊天被当作拍事件结构化决策");

    services.chat(json{{"text", ""}, {"poke", true}}.dump());
    auto first = poke(905, 77);
    first["time"] = 1700000011;
    deliver(bot, first);
    auto second = poke(905, 88);
    second["time"] = 1700000012;
    deliver(bot, second);
    chats += 2;
    pokes += 2;
    size_t first_seen = 0;
    size_t second_seen = 0;
    size_t outgoing_seen = 0;
    const auto consecutive = services.chats().back().at("messages");
    for (const auto& message : consecutive)
    {
        const auto record = modelContent(message);
        if (!record.is_object() || record.value("event_type", "") != "poke") continue;
        if (record.at("bot") == true) ++outgoing_seen;
        else if (record.at("user_id") == "77" && record.at("timestamp") == 1700000011) ++first_seen;
        else if (record.at("user_id") == "88" && record.at("timestamp") == 1700000012) ++second_seen;
    }
    require(first_seen == 1 && second_seen == 1 && outgoing_seen == 1, "连续无消息ID的拍事件重复、漏失或丢失已成功回拍");

    for (const auto& failure : {std::string("not-json"), std::string("[SILENT]"), json{{"text", ""}, {"poke", false}}.dump(),
            json{{"text", "INVALID_ACTION"}, {"poke", "yes"}}.dump()})
    {
        services.chat(failure);
        deliver(bot, poke(906));
        require(services.chats().size() == ++chats && services.sends().size() == ++sends && services.pokes().size() == pokes,
            "无效拍决策未发送失败提示或误执行动作");
        replyForm(services.sends().back());
        require(services.sends().back().dump().find("INVALID_ACTION") == std::string::npos, "无效拍决策被当作成功回复");
    }
    services.chat("MODEL_POKE_FAILURE", false, 503);
    deliver(bot, poke(907));
    require(services.chats().size() == ++chats && services.sends().size() == ++sends && services.pokes().size() == pokes,
        "拍请求失败未提示、自动重试或执行动作");
    replyForm(services.sends().back());

    services.chat(json{{"text", "POKE_TEXT_SUCCESS_ACTION_FAILED"}, {"poke", true}}.dump());
    services.pokeResponse(200, {{"status", "failed"}, {"retcode", 100}, {"data", nullptr}});
    deliver(bot, poke(908));
    require(services.pokes().size() == ++pokes && services.sends().size() == ++sends, "回拍失败阻断文字或自动重试");
    ++chats;
    services.pokeResponse();
    services.failSend(true);
    services.chat(json{{"text", "POKE_TEXT_FAILED_ACTION_SUCCESS"}, {"poke", true}}.dump());
    deliver(bot, poke(909));
    require(services.pokes().size() == ++pokes && services.sends().size() == ++sends, "文字失败阻断独立回拍或自动重试");
    ++chats;
    services.failSend(false);

    services.chat(json{{"text", ""}, {"poke", true}}.dump());
    for (const auto& result : {json{{"status", "ok"}, {"retcode", 0}}, json{{"status", "ok"}}, json{{"status", "ok"}, {"retcode", 1}}})
    {
        services.pokeResponse(result.contains("retcode") && result["retcode"] == 0 ? 500 : 200, result);
        deliver(bot, poke(910));
        ++chats;
        require(services.pokes().size() == ++pokes && services.sends().size() == sends, "失败动作自动重试或伪造文字回应");
    }
    services.pokeResponse();
    require(services.judges().size() == judges && services.chats().size() == chats, "拍路径调用判定或重复模型请求");

    const auto timeline_path = root / "data/AiModule/group_timeline.json";
    eventually([&] {
        const auto timelines = readJson(timeline_path).at("group_timeline");
        if (!timelines.contains("910") || timelines.at("910").size() != 3) return false;
        const auto verify = [&](const std::string& group, size_t text_count, size_t action_count) {
            size_t texts = 0;
            size_t actions = 0;
            for (const auto& record : timelines.at(group))
            {
                if (record.at("bot") != true) continue;
                if (record.value("event_type", "message") == "poke")
                {
                    ++actions;
                    require(record.at("user_id") == "42" && record.at("group_id") == group &&
                        record.at("target_id") == (group == "903" ? "88" : "77") &&
                        record.value("message_id", "").empty() && record.at("text") == "" && record.at("timestamp").get<int64_t>() > 0,
                        "成功回拍伪造成普通发言、伪造消息ID或缺少实际对象时间");
                }
                else ++texts;
            }
            return texts == text_count && actions == action_count;
        };
        return verify("901", 1, 0) && verify("902", 0, 1) && verify("903", 1, 1) && verify("908", 1, 0) &&
            verify("909", 0, 1) && verify("910", 0, 0);
    }, "拍动作和文字未按各自成功结果持久化");
    services.chat("AFTER_POKE_TESTS");
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
    auto incoming = std::async(std::launch::async, [&bot] { deliver(bot, event(json::array({{{"type", "record"}, {"data", {{"file", "CONCURRENT_LATER_VOICE"}}}}, text("")}), 456)); });
    const bool collected_during_generation = incoming.wait_for(1s) == std::future_status::ready;
    services.unblockChat();
    pending.get();
    incoming.get();
    require(collected_during_generation, "生成期间纯媒体入站被聊天请求阻塞");
    deliver(bot, event(json::array({at("42"), text("CONCURRENT_CHECK_INPUT")}), 456));
    messages = services.chats().back().at("messages").dump();
    require(messages.find("CONCURRENT_FIRST_INPUT") != std::string::npos && messages.find("CONCURRENT_SENT_REPLY") != std::string::npos, "并发历史丢失已接收消息或成功回复");
    require(messages.find("record") != std::string::npos, "生成期间新消息未保留媒体标记");
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
        deliver(bot, event(json::array({at("42"), text("RESTORED_IMAGE_QUERY")}), 822));
        require(imageUrls(services.chats().back().at("messages")) == json::array({"https://images.example/concurrent.png"}),
            "重启未恢复已保存图片URL或未重新作为图片输入");
        deliver(bot, event(json::array({at("42"), text("RESTORED_POKE_QUERY")}), 902));
        const auto restored_pokes = services.chats().back().at("messages");
        size_t incoming_pokes = 0;
        size_t outgoing_pokes = 0;
        for (const auto& message : restored_pokes)
        {
            const auto record = modelContent(message);
            if (!record.is_object() || record.value("event_type", "") != "poke") continue;
            if (record.at("bot") == true && record.at("target_id") == "77") ++outgoing_pokes;
            else if (record.at("bot") == false && record.at("target_id") == "42") ++incoming_pokes;
        }
        require(incoming_pokes == 1 && outgoing_pokes == 1, "重启未恢复真实拍事件及已确认回拍记录");
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
        multimodalAndReplies(services, root);
        std::cout << "图片多模态、来源隔离、回复形式通过\n";
        pokeActions(services, root);
        std::cout << "拍事件、动作约束、部分失败记录通过\n";
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
