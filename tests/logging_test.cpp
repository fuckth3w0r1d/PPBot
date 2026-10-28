#include "BotServer.h"
#include "config.h"
#include "logger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

static void require(bool value, const std::string& message)
{
    if(!value) throw std::runtime_error(message);
}

static std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static size_t count(const std::string& value, const std::string& needle)
{
    size_t result = 0;
    for(size_t position = 0; (position = value.find(needle, position)) != std::string::npos; position += needle.size()) ++result;
    return result;
}

static void oneWarning(const std::string& logs, const std::string& reason)
{
    require(count(logs, "[WARN]") == 1, "错误应只记录一条WARN: " + logs);
    require(logs.find(reason) != std::string::npos, "日志缺少错误原因 " + reason + ": " + logs);
    require(logs.find("判定结果无效") == std::string::npos, "已记录的判定失败产生重复告警");
}

class Services {
    struct Result { int status; std::string body; };
    httplib::Server server;
    std::thread worker;
    std::mutex mutex;
    std::map<std::string, Result> results;

public:
    int port;

    Services()
    {
        const auto handle = [this](const httplib::Request& request, httplib::Response& response) {
            auto path = request.path;
            if(path == "/chat")
            {
                const auto body = json::parse(request.body);
                const auto instructions = body.at("messages").at(0).at("content").get<std::string>();
                if(instructions.find("用户画像总结器") != std::string::npos) path = "/profile";
                if(instructions.find("群人格总结器") != std::string::npos) path = "/persona";
            }
            std::lock_guard<std::mutex> lock(mutex);
            const auto result = results.find(path);
            if(result != results.end())
            {
                response.status = result->second.status;
                response.set_content(result->second.body, "application/json");
            }
            else if(path == "/send_group_msg")
                response.set_content(json{{"status", "ok"}, {"retcode", 0}, {"data", {{"message_id", ++message_id}}}}.dump(), "application/json");
            else
                response.set_content(json{{"choices", json::array({{{"message", {{"content", path == "/judge" ? "NO" : "BOT_REPLY_PRIVATE_MARKER"}}}}})}}.dump(), "application/json");
        };
        server.Post("/.*", handle);
        server.Get("/.*", handle);
        port = server.bind_to_any_port("127.0.0.1");
        require(port > 0, "日志模拟服务无法绑定本地端口");
        worker = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    ~Services()
    {
        server.stop();
        if(worker.joinable()) worker.join();
    }

    void set(const std::string& path, int status, const std::string& body)
    {
        std::lock_guard<std::mutex> lock(mutex);
        results[path] = {status, body};
    }

    void clear(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(mutex);
        results.erase(path);
    }

private:
    int message_id = 8000;
};

static void configure(const Services& service, const std::filesystem::path& root)
{
    std::ifstream input("config.example.json");
    auto config = json::parse(input);
    config["log"]["file"]["path"] = (root / "events.log").string();
    config["log"]["file"]["level"] = 1;
    config["log"]["console"]["level"] = 4;
    config["file"]["data_path"] = (root / "data").string();
    config["file"]["cache_path"] = (root / "cache").string();
    config["file"]["save_frequency_seconds"] = 60;
    const auto config_path = root / "config.json";
    {
        std::ofstream output(config_path);
        output << config;
    }
    load_config(config_path.string());
    BOT_QQ = "42";
    BOT_NAME = "LogBot";
    SERVER_HOST = "127.0.0.1";
    SERVER_PORT = service.port;
    AI_HOST = "http://127.0.0.1:" + std::to_string(service.port);
    AI_PORT = service.port;
    JUDGE_HOST = AI_HOST;
    JUDGE_PORT = service.port;
    AI_POST_PATH = "/chat";
    JUDGE_POST_PATH = "/judge";
    AI_MAX_CHAT_ROUNDS = 100000;
    CHAT_REPLY_PROBABILITY = 0;
    AI_KEY = "ai-secret-for-log-test";
    JUDGE_KEY = "judge-secret-for-log-test";
    SERVER_TOKEN = "server-secret-for-log-test";
    CLIENT_TOKEN = "client-secret-for-log-test";
    AMAP_KEY = "amap-secret-for-log-test";
    MUSIC_HOST = SERVER_HOST;
    MUSIC_PORT = service.port;
    MUSIC_GET_ID_PATH = "/music-search";
    MUSIC_GET_URL_PATH = "/music-url";
    RANDOM_IMG_HOST2 = SERVER_HOST;
    RANDOM_IMG_PORT2 = service.port;
    RANDOM_IMG_GET_PATH2 = "/images";
}

static json event(const std::string& content, bool direct = false)
{
    static int id = 200;
    auto segments = json::array();
    if(direct) segments.push_back({{"type", "at"}, {"data", {{"qq", "42"}}}});
    segments.push_back({{"type", "text"}, {"data", {{"text", content}}}});
    return {{"post_type", "message"}, {"message_type", "group"}, {"group_id", 123}, {"user_id", 77}, {"message_id", ++id}, {"time", 1700000000}, {"sender", {{"nickname", "Member"}, {"card", "Card"}}}, {"message", segments}};
}

static void deliver(BotServer& bot, const json& body)
{
    httplib::Request request;
    httplib::Response response;
    request.body = body.dump();
    response.status = 200;
    bot.handlePost(request, response);
    require(response.status == 200, "受控错误响应导致上报处理异常");
}

static void formatter(const std::filesystem::path& log)
{
    size_t position = readText(log).size();
    Logger::httpError("formatter-nested", 429, json{{"error", {{"code", 1305}, {"type", "rate_limit"}, {"message", "nested-quota-reason"}}}, {"request_id", "request-nested-42"}}.dump());
    auto logs = readText(log).substr(position);
    oneWarning(logs, "nested-quota-reason");
    require(logs.find("429") != std::string::npos && logs.find("1305") != std::string::npos && logs.find("rate_limit") != std::string::npos && logs.find("request-nested-42") != std::string::npos, "嵌套服务错误字段丢失");
    require(count(logs, "\n") == 1, "单条HTTP错误输出多行");

    position = readText(log).size();
    Logger::httpError("formatter-fields", 402, json{{"code", 40201}, {"message", "balance-reason"}, {"msg", "msg-reason"}, {"info", "info-reason"}, {"infocode", "info-code-42"}, {"retcode", 100}, {"request_id", "request-top-42"}}.dump());
    logs = readText(log).substr(position);
    for(const auto& marker : {"40201", "balance-reason", "msg-reason", "info-reason", "info-code-42", "100", "request-top-42"})
        require(logs.find(marker) != std::string::npos, std::string("顶层服务错误字段丢失: ") + marker);

    position = readText(log).size();
    Logger::httpError("formatter-plain", 502, "<html>upstream-failed\nforged-log-line</html>");
    logs = readText(log).substr(position);
    oneWarning(logs, "upstream-failed");
    require(logs.find("\\nforged-log-line") != std::string::npos && count(logs, "\n") == 1, "非JSON错误正文伪造多行日志");

    position = readText(log).size();
    const std::string secrets = AI_KEY + " " + JUDGE_KEY + " " + SERVER_TOKEN + " " + CLIENT_TOKEN + " " + AMAP_KEY;
    Logger::httpError("formatter-secret", 500, secrets + "\r\n" + std::string(1480, 'x') + AI_KEY + std::string(2500, 'z'));
    Logger::httpError("boundary-key", 500, std::string(1530, 'x') + AI_KEY);
    std::string unicode;
    for(int i = 0; i < 900; ++i) unicode += "中";
    Logger::httpError("utf8-detail", 500, unicode + "\n");
    logs = readText(log).substr(position);
    for(const auto& secret : {AI_KEY, JUDGE_KEY, SERVER_TOKEN, CLIENT_TOKEN, AMAP_KEY})
        require(logs.find(secret) == std::string::npos, "配置密钥泄露到日志");
    require(logs.find(AI_KEY.substr(0, 6)) == std::string::npos, "截断边界泄露未先脱敏的密钥片段");
    std::istringstream lines(logs);
    for(std::string line; std::getline(lines, line);)
    {
        require(line.size() <= 1750, "日志详细内容未按字节限制");
        static_cast<void>(json(line).dump());
        require(line.find('\r') == std::string::npos, "日志保留裸回车");
    }
}

static void concurrentLines(const std::filesystem::path& log)
{
    const size_t position = readText(log).size();
    std::vector<std::thread> threads;
    for(int worker = 0; worker < 4; ++worker)
        threads.emplace_back([worker] {
            for(int entry = 0; entry < 20; ++entry)
                Logger::info("THREAD-" + std::to_string(worker) + "-" + std::to_string(entry), json{{"text", "first\nsecond"}, {"secret", AI_KEY}});
        });
    for(auto& thread : threads) thread.join();
    const auto logs = readText(log).substr(position);
    require(count(logs, "\n") == 80, "并发日志丢行或串行");
    std::istringstream lines(logs);
    for(std::string line; std::getline(lines, line);)
    {
        require(count(line, "THREAD-") == 1, "并发写入混合多条日志");
        require(line.find("first\\nsecond") != std::string::npos && line.find(AI_KEY) == std::string::npos, "并发JSON日志未紧凑转义或未脱敏");
    }
}

static void serviceErrors(Services& services, const std::filesystem::path& log)
{
    BotServer bot;
    const std::string quota = json{{"error", {{"code", 1305}, {"message", "quota-from-judge"}}}, {"request_id", "judge-request-id"}}.dump();
    services.set("/judge", 429, quota);
    size_t position = readText(log).size();
    deliver(bot, event("judge-failure-input"));
    auto logs = readText(log).substr(position);
    oneWarning(logs, "quota-from-judge");
    require(logs.find("429") != std::string::npos && logs.find("1305") != std::string::npos && logs.find("judge-request-id") != std::string::npos, "判定HTTP错误缺少服务细节");
    services.clear("/judge");

    services.set("/chat", 402, json{{"message", "account-balance-empty"}, {"request_id", "chat-request-id"}, {"key", AI_KEY}}.dump());
    position = readText(log).size();
    deliver(bot, event("direct-failure-input", true));
    logs = readText(log).substr(position);
    oneWarning(logs, "account-balance-empty");
    require(logs.find("402") != std::string::npos && logs.find("chat-request-id") != std::string::npos && logs.find(AI_KEY) == std::string::npos, "聊天HTTP错误缺少原因或泄露密钥");
    services.clear("/chat");

    for(const auto& body : {std::string("not-json\nplain-error"), std::string("<html>gateway-error</html>"), std::string("{}")})
    {
        services.set("/judge", 200, body);
        position = readText(log).size();
        deliver(bot, event("invalid-response-input"));
        oneWarning(readText(log).substr(position), "200");
    }
    services.clear("/judge");

    services.set("/send_group_msg", 200, json{{"status", "failed"}, {"retcode", 1400}, {"message", "napcat-send-denied"}, {"token", SERVER_TOKEN}}.dump());
    position = readText(log).size();
    deliver(bot, event("send-failure-input", true));
    logs = readText(log).substr(position);
    oneWarning(logs, "napcat-send-denied");
    require(logs.find("1400") != std::string::npos && logs.find(SERVER_TOKEN) == std::string::npos, "NapCat业务错误详情缺失或泄露token");
    services.clear("/send_group_msg");

    AI_MAX_CHAT_ROUNDS = 10;
    services.set("/profile", 402, "{\"message\":\"profile-balance-error\"}");
    services.set("/persona", 402, "{\"message\":\"persona-balance-error\"}");
    position = readText(log).size();
    deliver(bot, event("summary-errors-input", true));
    logs = readText(log).substr(position);
    require(count(logs, "[WARN]") == 2 && logs.find("profile-balance-error") != std::string::npos && logs.find("persona-balance-error") != std::string::npos, "画像与人格失败未分别记录或重复记录");
    require(logs.find("画像") != std::string::npos && logs.find("人格") != std::string::npos, "总结操作名称不能区分");
    AI_MAX_CHAT_ROUNDS = 100000;

    position = readText(log).size();
    deliver(bot, event("GROUP_MESSAGE_PRIVATE_MARKER", true));
    logs = readText(log).substr(position);
    require(logs.find("GROUP_MESSAGE_PRIVATE_MARKER") == std::string::npos && logs.find("BOT_REPLY_PRIVATE_MARKER") == std::string::npos, "INFO日志包含群消息或回复正文");
    require(logs.find("null") == std::string::npos && logs.find("[WARN]") == std::string::npos, "成功摘要打印null或告警");
    position = readText(log).size();
    deliver(bot, event("ordinary-silent-message"));
    require(readText(log).substr(position).find("[WARN]") == std::string::npos, "正常沉默被当作错误");
}

static void commandsAndFiles(Services& services, const std::filesystem::path& log, const std::filesystem::path& root)
{
    MusicCommand music;
    RandomImgCommand2 images;
    for(const auto& endpoint : {std::string("/music-search"), std::string("/images")})
    {
        for(const auto& body : {std::string("<html>command-upstream-error</html>"), std::string("{}")})
        {
            services.set(endpoint, body.front() == '<' ? 429 : 200, body);
            const size_t position = readText(log).size();
            if(endpoint == "/music-search") music.execute("test-song");
            else images.execute("test-tag");
            oneWarning(readText(log).substr(position), body.front() == '<' ? "command-upstream-error" : "200");
        }
    }
    const size_t position = readText(log).size();
    require(!FileManager::writeJsonFile((root / "missing/data.json").string(), json::object()), "不可写路径意外成功");
    const auto logs = readText(log).substr(position);
    require(logs.find("missing/data.json") != std::string::npos && (logs.find("No such file") != std::string::npos || logs.find("不存在") != std::string::npos), "文件打开失败缺少系统原因或路径");
    const auto original_log = LOG_FILE_PATH;
    LOG_FILE_PATH = (root / "missing/events.log").string();
    std::ostringstream diagnostic;
    const auto stream = std::cerr.rdbuf(diagnostic.rdbuf());
    Logger::warn("log-write-failure", "test-detail");
    std::cerr.rdbuf(stream);
    LOG_FILE_PATH = original_log;
    require(diagnostic.str().find("events.log") != std::string::npos && (diagnostic.str().find("No such file") != std::string::npos || diagnostic.str().find("不存在") != std::string::npos), "日志文件打开失败未输出路径和系统原因");
}

int main()
{
    const std::filesystem::path root = "release/tests/logging-runtime";
    bool created = false;
    try
    {
        require(!std::filesystem::exists(root), "日志测试目录已存在");
        std::filesystem::create_directories(root);
        created = true;
        Services services;
        configure(services, root);
        const std::filesystem::path log = LOG_FILE_PATH;
        formatter(log);
        concurrentLines(log);
        serviceErrors(services, log);
        commandsAndFiles(services, log, root);
        std::filesystem::remove_all(root);
        std::cout << "日志格式、错误细节、脱敏、并发及业务边界验收通过\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "日志测试失败: " << error.what() << '\n';
        if(created) std::filesystem::remove_all(root);
        return 1;
    }
}
