#include "BotServer.h"
#include "config.h"
#include "sqlite3.h"

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
#include <ctime>
#include <random>
#include <cstdlib>
#include <openssl/x509v3.h>

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

static json readTimeline(const std::filesystem::path& directory)
{
    sqlite3* database = nullptr;
    const auto path = directory / "messages.sqlite3";
    const auto opened = sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr);
    if (opened != SQLITE_OK)
    {
        if (database) sqlite3_close(database);
        return json::object();
    }
    sqlite3_stmt* statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(database, "SELECT record FROM messages ORDER BY sequence", -1, &statement, nullptr);
    json groups = json::object();
    if (prepared == SQLITE_OK)
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const auto record = json::parse(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
            auto& history = groups[record.at("group_id").get<std::string>()];
            if (history.is_null()) history = json::array();
            history.push_back(record);
        }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return groups;
}

static size_t estimatedTokens(const json& messages)
{
    const auto textTokens = [](const std::string& value) {
        size_t ascii = 0;
        size_t non_ascii = 0;
        for (const auto byte : value)
            if (static_cast<unsigned char>(byte) < 0x80) ++ascii;
            else if ((static_cast<unsigned char>(byte) & 0xc0) != 0x80) ++non_ascii;
        return (ascii + 3) / 4 + 2 * non_ascii;
    };
    size_t total = 0;
    for (const auto& message : messages)
    {
        total += 8;
        const auto& content = message.at("content");
        if (content.is_string()) total += textTokens(content.get<std::string>());
        else
            for (const auto& part : content)
                if (part.at("type") == "image_url") total += 4096;
                else if (part.at("type") == "text") total += textTokens(part.at("text").get<std::string>());
    }
    return total;
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

static std::string streamReply(const std::string& content)
{
    std::string result = ": heartbeat\r\n\r\n";
    const auto append = [&](const json& value) { result += "data: " + value.dump() + "\r\n\r\n"; };
    append({{"choices", json::array({{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", nullptr}, {"reasoning_content", "HIDDEN_REASONING_MARKER"}}}}})}});
    for (size_t offset = 0; offset < content.size();)
    {
        size_t end = std::min(offset + 12, content.size());
        while (end < content.size() && (static_cast<unsigned char>(content[end]) & 0xc0) == 0x80) ++end;
        append({{"choices", json::array({{{"index", 0}, {"delta", {{"content", content.substr(offset, end - offset)}}}}})}});
        offset = end;
    }
    append({{"choices", json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}})}});
    append({{"choices", json::array()}, {"usage", {{"total_tokens", 99}}}});
    return result + "data: [DONE]\r\n\r\n";
}

class Services {
    httplib::Server server;
    std::thread worker;
    std::unique_ptr<httplib::SSLServer> secure_server;
    std::thread secure_worker;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<json> judge_requests;
    std::vector<json> chat_requests;
    std::vector<json> sent_requests;
    std::vector<json> poke_requests;
    std::vector<json> command_requests;
    bool command_failure = false;
    std::string previous_ca;
    bool had_ca = false;
    std::string judge_result = "NO";
    std::string chat_result = "CHAT_DEFAULT";
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
    std::string stream_override;
    bool disconnect_stream = false;
    int stream_delay_ms = 0;
    bool block_stream = false;
    bool stream_entered = false;
    bool release_stream = false;
    size_t fragment_size = 0;
    int fragment_delay_ms = 1;

public:
    int port;
    int secure_port;

    Services()
    {
        server.Post("/embeddings", [](const httplib::Request& request, httplib::Response& response) {
            const auto body = json::parse(request.body);
            json results = json::array();
            const auto inputs = body.at("input").is_array() ? body.at("input") : json::array({body.at("input")});
            for (size_t i = 0; i < inputs.size(); ++i)
            {
                std::mt19937 generator(static_cast<unsigned>(std::hash<std::string>{}(inputs[i].get<std::string>())));
                json vector = json::array();
                for (size_t j = 0; j < 64; ++j) vector.push_back((generator() & 1) ? 0.125 : -0.125);
                results.push_back({{"index", i}, {"embedding", vector}});
            }
            response.set_content(json{{"data", results}}.dump(), "application/json");
        });
        server.Post("/judge", [this](const httplib::Request& request, httplib::Response& response) {
            std::string result;
            bool invalid;
            int delay;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto body = json::parse(request.body);
                require(!body.value("stream", false), "判定请求错误启用流式");
                require(body.value("thinking", json::object()) == json{{"type", "disabled"}}, "判定请求未关闭思考模式");
                judge_requests.push_back(body);
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
            const auto body = json::parse(request.body);
            require(body.value("stream", false), "主模型请求未启用流式");
            require(!body.contains("thinking"), "主模型请求携带了判定专用思考参数");
            chat_requests.push_back(body);
            if (block_chat)
            {
                chat_entered = true;
                condition.notify_all();
                condition.wait(lock, [this] { return release_chat; });
                block_chat = false;
            }
            const std::string result = chat_result;
            const bool invalid = chat_invalid;
            const int status = chat_status;
            std::string data = invalid ? "data: not-json\r\n\r\ndata: [DONE]\r\n\r\n" : streamReply(result);
            if (!stream_override.empty()) data = stream_override;
            const bool disconnect = disconnect_stream;
            const int delay = stream_delay_ms;
            const bool pause = block_stream;
            const size_t fragment = fragment_size;
            const int interval = fragment_delay_ms;
            lock.unlock();
            response.status = status;
            if (status != 200)
            {
                response.set_content(result, "application/json");
                return;
            }
            response.set_chunked_content_provider("text/event-stream",
                [this, data, disconnect, delay, pause, fragment, interval](size_t, httplib::DataSink& sink) {
                    const auto done = data.find("data: [DONE]");
                    const size_t split = (pause || delay) && done != std::string::npos ? done : data.size();
                    const size_t step = fragment ? fragment : std::max<size_t>(1, split);
                    for (size_t offset = 0; offset < split; offset += step)
                    {
                        if (!sink.write(data.data() + offset, std::min(step, split - offset))) return false;
                        if (fragment) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                    }
                    if (pause)
                    {
                        std::unique_lock<std::mutex> stream_lock(mutex);
                        stream_entered = true;
                        condition.notify_all();
                        condition.wait(stream_lock, [this] { return release_stream; });
                        block_stream = false;
                    }
                    if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    if (disconnect) return false;
                    if (split < data.size() && !sink.write(data.data() + split, data.size() - split)) return false;
                    sink.done();
                    return true;
                });
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
        server.Get("/command/.*", [this](const httplib::Request& request, httplib::Response& response) {
            command(request, response);
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
            release_stream = true;
            condition.notify_all();
        }
        server.stop();
        if (worker.joinable()) worker.join();
        if (secure_server)
        {
            secure_server->stop();
            if (secure_worker.joinable()) secure_worker.join();
            if (had_ca) setenv("SSL_CERT_FILE", previous_ca.c_str(), 1);
            else unsetenv("SSL_CERT_FILE");
        }
    }

    void command(const httplib::Request& request, httplib::Response& response)
    {
        std::lock_guard<std::mutex> lock(mutex);
        command_requests.push_back({{"path", request.path}, {"city", request.get_param_value("city")},
            {"keywords", request.get_param_value("keywords")}, {"tag", request.get_param_value("tag")}});
        json result;
        if (command_failure) result = {{"status", "0"}};
        else if (request.path == "/command/weather") result = {{"status", "1"}, {"lives", json::array({{
            {"province", "北京"}, {"city", "北京"}, {"weather", "晴"}, {"temperature", "20"},
            {"winddirection", "东"}, {"windpower", "2"}, {"humidity", "50"}, {"reporttime", "test-time"}}})}};
        else if (request.path == "/command/meal") result = {{"mealwhat", "MEAL_RESULT"}};
        else if (request.path == "/command/image") result = {{"url", "https://example.com/one.png"}};
        else if (request.path == "/command/images") result = {{"proxyUrls", json::array({"https://example.com/two.png", "https://example.com/three.png"})}};
        else if (request.path == "/command/music-search") result = {{"result", {{"songs", json::array({{{"id", 321}}})}}}};
        else result = {{"data", json::array({{{"url", "https://example.com/song.mp3"}}})}};
        response.set_content(result.dump(), "application/json");
    }

    void secureCommands(const std::filesystem::path& root)
    {
        const auto certificate_path = (root / "command-ca.pem").string();
        secure_server = std::make_unique<httplib::SSLServer>([&](SSL_CTX& context) {
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048), EVP_PKEY_free);
            std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
            require(key && certificate, "无法生成测试证书");
            require(X509_set_version(certificate.get(), 2) && ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) &&
                X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) &&
                X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) && X509_set_pubkey(certificate.get(), key.get()),
                "无法初始化测试证书");
            auto name = X509_get_subject_name(certificate.get());
            require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0) && X509_set_issuer_name(certificate.get(), name),
                "无法设置测试证书名称");
            for (const auto& extension : {std::make_pair(NID_subject_alt_name, "IP:127.0.0.1"), std::make_pair(NID_basic_constraints, "critical,CA:TRUE")})
            {
                std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> value(
                    X509V3_EXT_conf_nid(nullptr, nullptr, extension.first, const_cast<char*>(extension.second)), X509_EXTENSION_free);
                require(value && X509_add_ext(certificate.get(), value.get(), -1), "无法设置测试证书扩展");
            }
            require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0 &&
                SSL_CTX_use_certificate(&context, certificate.get()) && SSL_CTX_use_PrivateKey(&context, key.get()),
                "无法签发测试证书");
            std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(certificate_path.c_str(), "w"), BIO_free);
            require(output && PEM_write_bio_X509(output.get(), certificate.get()), "无法保存测试证书");
            return true;
        });
        require(secure_server->is_valid(), "HTTPS 模拟服务初始化失败");
        const char* original = std::getenv("SSL_CERT_FILE");
        had_ca = original != nullptr;
        if (original) previous_ca = original;
        require(setenv("SSL_CERT_FILE", certificate_path.c_str(), 1) == 0, "无法信任测试证书");
        secure_server->Get("/command/.*", [this](const httplib::Request& request, httplib::Response& response) {
            command(request, response);
        });
        secure_port = secure_server->bind_to_any_port("127.0.0.1");
        require(secure_port > 0, "HTTPS 模拟服务无法绑定端口");
        secure_worker = std::thread([this] { secure_server->listen_after_bind(); });
        eventually([this] { return secure_server->is_running(); }, "HTTPS 模拟服务未启动");
    }

    void failCommand(bool fail) { std::lock_guard<std::mutex> lock(mutex); command_failure = fail; }
    std::vector<json> commands() { std::lock_guard<std::mutex> lock(mutex); return command_requests; }

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

    void stream(const std::string& data = "", size_t fragment = 0, bool disconnect = false, int delay = 0, int interval = 1)
    {
        std::lock_guard<std::mutex> lock(mutex);
        stream_override = data;
        fragment_size = fragment;
        disconnect_stream = disconnect;
        stream_delay_ms = delay;
        fragment_delay_ms = interval;
    }

    void pauseStream()
    {
        std::lock_guard<std::mutex> lock(mutex);
        block_stream = true;
        stream_entered = false;
        release_stream = false;
    }

    void awaitStream()
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, 5s, [this] { return stream_entered; }), "流式响应未进入可控阻塞点");
    }

    void unblockStream()
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_stream = true;
        condition.notify_all();
    }

    void failSend(bool fail)
    {
        std::lock_guard<std::mutex> lock(mutex);
        send_failure = fail;
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
        {"group_id", group}, {"user_id", user}, {"target_id", target}, {"time", static_cast<int64_t>(std::time(nullptr))}};
}

static json event(const json& segments, int group = 123, int user = 77)
{
    static std::atomic<int> id{100};
    return {{"post_type", "message"}, {"message_type", "group"}, {"group_id", group}, {"user_id", user}, {"message_id", ++id}, {"time", static_cast<int64_t>(std::time(nullptr))}, {"sender", {{"user_id", user}, {"nickname", "member_" + std::to_string(user)}, {"card", "card_" + std::to_string(user)}}}, {"message", segments}};
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
    CHAT_LLM_HOST = "http://127.0.0.1:" + std::to_string(services.port);
    CHAT_LLM_PORT = services.port;
    CHAT_LLM_POST_PATH = "/chat";
    CHAT_LLM_MODEL = "chat-test";
    CHAT_LLM_KEY = "chat-key";
    CHAT_LLM_SYS_PROMPTS = "SYSTEM_PERSONA_MARKER";
    CHAT_LLM_MAX_TOKENS = 64;
    CHAT_LLM_MAX_MESSAGES = 120;
    CHAT_LLM_CONTEXT_TOKENS = 32000;
    CHAT_LLM_TIMEOUT_SECONDS = 5;
    JUDGE_HOST = CHAT_LLM_HOST;
    JUDGE_PORT = services.port;
    JUDGE_POST_PATH = "/judge";
    JUDGE_MODEL = "judge-test";
    JUDGE_KEY = "judge-key";
    JUDGE_MAX_TOKENS = 16;
    JUDGE_TIMEOUT_SECONDS = 1;
    JUDGE_MAX_MESSAGES = 12;
    JUDGE_CONTEXT_TOKENS = 4000;
    EMBEDDING_HOST = CHAT_LLM_HOST;
    EMBEDDING_PORT = services.port;
    EMBEDDING_POST_PATH = "/embeddings";
    EMBEDDING_MODEL = "embedding-test";
    EMBEDDING_KEY = "embedding-key";
    EMBEDDING_TIMEOUT_SECONDS = 1;
    CHAT_LLM_RETRIEVAL_CONTEXT_TOKENS = 2048;
    CHAT_LLM_REPLY_PROBABILITY = 0;
    B23_APPID = "test-b23";
    B23_HOST = "127.0.0.1";
    B23_PORT = 1;
    B23_GET_QUERY_PATH = "/never";
    std::filesystem::create_directories(root / "data/AiModule");
}

static void configuration(const std::filesystem::path& root)
{
    json settings = readJson("config.example.json");
    require(!settings.contains("ai"), "示例配置仍使用旧聊天配置块");
    require(!settings.contains("ai2"), "示例配置仍要求第二聊天模型");
    require(settings.at("chat_llm").at("reply_probability") == 0.1, "示例插话概率未设为10%");
    settings["chat_llm"]["reply_probability"] = 0.37;
    const auto path = root / "config.json";
    writeJson(path, settings);
    load_config(path.string());
    require(CHAT_LLM_REPLY_PROBABILITY == 0.37, "插话概率配置未生效");
    auto legacy = settings;
    legacy["ai"] = legacy.at("chat_llm");
    legacy.erase("chat_llm");
    writeJson(path, legacy);
    bool legacy_rejected = false;
    try { load_config(path.string()); } catch(const json::out_of_range& error) {
        legacy_rejected = std::string(error.what()).find("chat_llm") != std::string::npos;
    }
    require(legacy_rejected, "旧ai配置块没有要求改为chat_llm");
    require(!settings.at("chat_llm").contains("max_rounds") && !settings.at("chat_llm").contains("context_bytes") &&
        !settings.at("judge").contains("context_bytes"), "示例仍含已停用参数");
    require(CHAT_LLM_CONTEXT_TOKENS == settings.at("chat_llm").at("context_tokens") &&
        JUDGE_CONTEXT_TOKENS == settings.at("judge").at("context_tokens"), "token预算加载失败");
    settings["chat_llm"]["max_rounds"] = "ignored-legacy-value";
    settings["chat_llm"]["context_bytes"] = "ignored-legacy-value";
    settings["judge"]["context_bytes"] = "ignored-legacy-value";
    settings["embedding"]["model"] = "embedding-config-test";
    settings["embedding"]["path"] = "/embedding-config-test";
    writeJson(path, settings);
    load_config(path.string());
    require(EMBEDDING_MODEL == "embedding-config-test" && EMBEDDING_POST_PATH == "/embedding-config-test", "embedding独立配置未生效");
    const auto valid = settings;
    for (const auto& invalid : {json(-1), json(0), json(1.5)})
    {
        settings = valid;
        settings["chat_llm"]["context_tokens"] = invalid;
        writeJson(path, settings);
        bool rejected = false;
        try { load_config(path.string()); } catch (const std::exception&) { rejected = true; }
        require(rejected, "无效token预算未拒绝");
    }
    settings = valid;
    settings["chat_llm"]["max_tokens"] = settings["chat_llm"]["context_tokens"];
    writeJson(path, settings);
    bool no_input = false;
    try { load_config(path.string()); } catch (const std::exception&) { no_input = true; }
    require(no_input, "没有输入空间的总预算未拒绝");
    settings = valid;
    settings["chat_llm"]["reply_probability"] = 1.01;
    writeJson(path, settings);
    bool rejected = false;
    try { load_config(path.string()); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "超出范围的插话概率未拒绝");
}

static void commandRouting(Services& services, const std::filesystem::path& root)
{
    services.secureCommands(root);
    AMAP_HOST = EAT_HOST = RANDOM_IMG_HOST = "127.0.0.1";
    AMAP_PORT = EAT_PORT = RANDOM_IMG_PORT = services.secure_port;
    AMAP_GET_PATH = "/command/weather";
    EAT_GET_PATH = "/command/meal";
    RANDOM_IMG_GET_PATH = "/command/image";
    RANDOM_IMG_HOST2 = MUSIC_HOST = "127.0.0.1";
    RANDOM_IMG_PORT2 = MUSIC_PORT = services.port;
    RANDOM_IMG_GET_PATH2 = "/command/images";
    MUSIC_GET_ID_PATH = "/command/music-search";
    MUSIC_GET_URL_PATH = "/command/music-url";
    services.judge("NO");
    services.chat("COMMAND_CHAT_FALLTHROUGH");
    CmdTaskManager commands;
    BotServer bot;
    const auto invoke = [&](const json& segments) {
        const auto judges = services.judges().size();
        const auto chats = services.chats().size();
        const auto sends = services.sends().size();
        const auto input = event(segments, 700);
        deliver(bot, input);
        require(services.judges().size() == judges && services.chats().size() == chats, "合法命令请求了模型");
        require(services.sends().size() == sends + 1, "合法命令未发送一条回复");
        const auto sent = services.sends().back();
        if(sent.contains("message") && sent.at("message").back().at("type") == "text")
            replyForm(sent, input.at("message_id").dump());
        return sent;
    };
    const auto help = invoke(json::array({at("42"), text(" /帮助")}));
    const auto help_text = help.at("message").back().at("data").at("text").get<std::string>();
    require(help.at("message").size() == 2 && help_text.find("显式 @ 本机器人") != std::string::npos &&
        help_text.find("@机器人 /命令 参数") != std::string::npos, "帮助未说明新格式或改变了回复形式");
    for (const auto& name : {"帮助", "时间", "天气", "吃啥", "随机二次元图片", "来点涩图", "点歌"})
        require(occurrences(help_text, std::string("/") + name) == 1, "帮助列表缺少命令或重复命令");
    const auto help_id = services.lastMessageId();
    for (const auto& segments : {
        json::array({at("42"), text("/时间")}),
        json::array({text("/时间 "), at("42")}),
        json::array({text("    /时间"), at(42)}),
        json::array({at("42"), text(" /时"), image("https://example.com/input.png"), text("间")}),
        json::array({text(" /时"), at("42"), text("间")}),
        json::array({reply(help_id), text("/时间"), at("42")})})
    {
        const auto result = invoke(segments);
        require(result.at("message").size() == 2 && result.at("message").back().at("type") == "text" &&
            result.at("message").back().at("data").at("text").get<std::string>() != "COMMAND_CHAT_FALLTHROUGH",
            "@ 位置或文本合并导致时间命令进入聊天");
    }
    for(const auto& id : {json(nullptr), json(0), json("invalid")})
    {
        auto input = event(json::array({at("42"), text("/时间")}), 700);
        if(id.is_null()) input.erase("message_id");
        else input["message_id"] = id;
        deliver(bot, input);
        replyForm(services.sends().back());
    }
    auto negative_id = event(json::array({at("42"), text("/时间")}), 700);
    negative_id["message_id"] = -9001;
    deliver(bot, negative_id);
    replyForm(services.sends().back(), "-9001");
    const json card = {{"meta", {{"detail_1", {{"appid", B23_APPID}, {"qqdocurl", "invalid-short-link"}}}}}};
    const auto priority = invoke(json::array({at("42"), text("/时间"),
        {{"type", "json"}, {"data", {{"data", card.dump()}}}}}));
    require(priority.at("message").size() == 2 && priority.at("message").back().at("type") == "text" &&
        priority.dump().find("COMMAND_CHAT_FALLTHROUGH") == std::string::npos && priority.dump().find("失败") == std::string::npos,
        "合法命令未优先于 B 站卡片");

    for (const auto& segments : {
        json::array({text("时间")}), json::array({text("/时间")}),
        json::array({at("77"), text("/时间")}), json::array({at("all"), text("/时间")}),
        json::array({text("@机器人 /时间")})})
    {
        const auto judges = services.judges().size();
        const auto chats = services.chats().size();
        const auto sends = services.sends().size();
        const auto context = MessageManager::getMessageContext(event(segments, 700));
        require(!commands.canHandle(context) && commands.handleTask(context).first.empty(), "无实际 @ 的消息匹配命令");
        deliver(bot, event(segments, 700));
        require(services.judges().size() == judges + 1 && services.chats().size() == chats && services.sends().size() == sends,
            "裸命令或无实际 @ 消息未沿用普通聊天分流");
    }
    for (const auto& value : {"时间", "正文 /时间", "/未知命令", "/", "/ 时间", "//时间", "/时间更多",
        "\t/时间", "/时间\t参数", "／时间", "/时间\n"})
    {
        const auto judges = services.judges().size();
        const auto chats = services.chats().size();
        const auto segments = json::array({at("42"), text(value)});
        require(!commands.canHandle(MessageManager::getMessageContext(event(segments, 700))), "非法命令格式匹配成功");
        deliver(bot, event(segments, 700));
        require(services.judges().size() == judges && services.chats().size() == chats + 1 &&
            services.sends().back().at("message").back().at("data").at("text") == "COMMAND_CHAT_FALLTHROUGH",
            "@ 的非命令消息未继续直接聊天");
    }
    for (const auto& segments : {json::array({text("正文 "), at("42"), text(" /时间")}),
        json::array({reply(help_id), text("/时间")})})
    {
        const auto chats = services.chats().size();
        const auto judges = services.judges().size();
        deliver(bot, event(segments, 700));
        require(services.chats().size() == chats + 1 && services.judges().size() == judges &&
            services.sends().back().at("message").back().at("data").at("text") == "COMMAND_CHAT_FALLTHROUGH",
            "正文前缀或仅引用消息误执行命令");
    }
    auto private_event = event(json::array({at("42"), text("/时间")}), 700);
    private_event["message_type"] = "private";
    for (const auto& input : {private_event, event(json::array({at("42"), text("/时间")}), 700, 42)})
    {
        const auto judges = services.judges().size();
        const auto chats = services.chats().size();
        const auto sends = services.sends().size();
        require(!commands.canHandle(MessageManager::getMessageContext(input)), "私聊或自身消息匹配命令");
        deliver(bot, input);
        require(services.judges().size() == judges && services.chats().size() == chats && services.sends().size() == sends,
            "私聊或自身消息新增业务回复");
    }

    const auto external = services.commands().size();
    const auto missing_weather = invoke(json::array({at("42"), text("/天气   ")}));
    require(missing_weather.dump().find("@机器人 /天气 城市名称") != std::string::npos, "天气缺参数提示未使用新格式");
    const auto missing_music = invoke(json::array({at("42"), text("/点歌")}));
    require(missing_music.at("message").back().at("data").at("text") == "请输入歌曲名称" &&
        services.commands().size() == external, "缺少参数访问外部服务或改变点歌提示");
    const auto weather = invoke(json::array({at("42"), text("/天气 北京")}));
    require(services.commands().back().at("city") == "北京" && weather.dump().find("北京 北京") != std::string::npos,
        "天气参数或业务回复改变");
    const auto music_event = event(json::array({at("42"), text("   /点"),
        image("https://example.com/music-input.png"), text("歌   歌曲 名称")}), 700);
    const auto original_context = MessageManager::getMessageContext(music_event);
    const auto saved_segments = original_context.msg_segments;
    const auto saved_text = original_context.pmsgsegs.text;
    require(commands.canHandle(original_context), "合并后的点歌命令未识别");
    const auto judges = services.judges().size();
    const auto chats = services.chats().size();
    deliver(bot, music_event);
    require(services.judges().size() == judges && services.chats().size() == chats, "点歌命令请求了模型");
    const auto requests = services.commands();
    require(requests.at(requests.size() - 2).at("keywords") == "歌曲 名称" &&
        services.sends().back().at("message") == json::array({MessageManager::buildMsg("record", "https://example.com/song.mp3")}),
        "点歌参数空格或语音回复改变");
    const auto meal = invoke(json::array({at("42"), text("/吃啥")}));
    require(meal.at("message").back() == text("MEAL_RESULT"), "餐食业务结果改变");
    const auto random_image = invoke(json::array({at("42"), text("/随机二次元图片")}));
    require(random_image.at("messages").size() == 1 &&
        random_image.at("messages").at(0).at("data").at("content") ==
            json::array({MessageManager::buildMsg("image", "https://example.com/one.png")}), "随机图片未使用合并转发图片");
    const auto tagged_images = invoke(json::array({at("42"), text("/来点涩图   tag one")}));
    require(services.commands().back().at("tag") == "tag one" && tagged_images.at("messages").size() == 2 &&
        tagged_images.at("messages").at(1).at("data").at("content") ==
            json::array({MessageManager::buildMsg("image", "https://example.com/three.png")}), "标签参数或图片转发改变");

    services.failCommand(true);
    const auto failed_weather = invoke(json::array({at("42"), text("/天气 北京")}));
    require(failed_weather.dump().find("@机器人 /天气 城市名称") != std::string::npos, "天气查询失败提示仍使用裸命令");
    for (const auto& scenario : {std::make_pair("/吃啥", "餐食查询失败"), std::make_pair("/随机二次元图片", "随机图片获取失败"),
        std::make_pair("/来点涩图", "标签图片获取失败"), std::make_pair("/点歌 歌曲", "搜索歌曲失败")})
        require(invoke(json::array({at("42"), text(scenario.first)})).dump().find(scenario.second) != std::string::npos,
            "外部服务失败未保留原提示");
    services.failCommand(false);
    services.failSend(true);
    const auto failed_event = event(json::array({at("42"), text("/时间")}), 700);
    const auto failed_id = std::stoll(services.lastMessageId()) + 1;
    deliver(bot, failed_event);
    services.failSend(false);
    const auto success_event = event(json::array({at("42"), text("/时间")}), 700);
    deliver(bot, success_event);
    const auto success_id = services.lastMessageId();
    require(bot.tsk_manager.shutdown(), "命令历史最终归档失败");
    const auto history = readTimeline(root / "data/AiModule").at("700");
    bool original_recorded = false;
    bool failed_input_recorded = false;
    bool successful_reply_recorded = false;
    for (const auto& record : history)
    {
        if (record.at("message_id") == music_event.at("message_id").dump())
            original_recorded = record.at("segments") == saved_segments && record.at("text") == saved_text;
        if (record.at("message_id") == failed_event.at("message_id").dump()) failed_input_recorded = true;
        if (record.at("bot") == true)
        {
            require(record.at("message_id") != std::to_string(failed_id), "发送失败的命令回复进入历史");
            if (record.at("message_id") == success_id)
                successful_reply_recorded = record.at("reply_to") == success_event.at("message_id").dump() &&
                    record.at("segments").front() == reply(success_event.at("message_id").dump());
        }
    }
    require(original_recorded && failed_input_recorded && successful_reply_recorded,
        "命令原消息被改写、发送失败丢失入站或成功出站未记录");
    services.chat("CHAT_DEFAULT");
}

static void routingAndHistory(Services& services)
{
    BotServer bot;
    size_t judges = services.judges().size();
    size_t chats = services.chats().size();
    size_t sends = services.sends().size();
    deliver(bot, event(json::array({at("42"), text(" /时间")})));
    require(services.sends().size() == sends + 1, "@ /时间未由命令回复");
    require(services.judges().size() == judges && services.chats().size() == chats, "命令被聊天模型抢占");

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
    require(services.sends().size() == sends + 1, "仅@未收到聊天回复");
    replyForm(services.sends().back(), empty_mention.at("message_id").dump());
    require(services.judges().size() == judges && services.chats().size() == ++chats, "仅@未直接进入聊天");
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
    require(shared.find("GROUP_123_PERSONA_MARKER") == std::string::npos && shared.find("GROUP_123_PROFILE_MARKER") == std::string::npos, "已停用群人格或画像仍被注入");
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
        const auto record = modelContent(message);
        if (record.is_object() && record.value("message_id", "") == cleared_card.at("message_id").dump())
            cleared = record.at("card") == "" && record.at("nickname") == "member_77" && record.at("user_id") == "77";
    }
    require(cleared, "当前消息未保留实际昵称、发送者和已清空群名片");

    services.judge("YES");
    deliver(bot, event(json::array({text("JUDGE_YES_MARKER")})));
    replyForm(services.sends().back());
    require(services.judges().size() == ++judges && services.chats().size() == ++chats, "YES未触发主模型");
    const std::string judge = services.judges().back().at("messages").dump();
    require(occurrences(judge, "JUDGE_YES_MARKER") == 1, "判定当前消息重复");
    require(judge.find("DIRECT_REPLY_MARKER") != std::string::npos || judge.find("SHARED_REPLY_MARKER") != std::string::npos, "判定短上下文缺失bot回复");
    require(judge.find("SYSTEM_PERSONA_MARKER") != std::string::npos && judge.find("GROUP_123_PERSONA_MARKER") == std::string::npos, "判定缺少静态人设或仍注入群人格");
    require(judge.find("GROUP_123_PROFILE_MARKER") == std::string::npos, "判定携带长期用户画像");

    services.judge("NO");
    sends = services.sends().size();
    deliver(bot, event(json::array({text("NO_SILENT_MARKER")})));
    require(services.judges().size() == ++judges && services.chats().size() == chats && services.sends().size() == sends, "NO且概率0未沉默");
    CHAT_LLM_REPLY_PROBABILITY = 1;
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
        CHAT_LLM_REPLY_PROBABILITY = 0;
        deliver(bot, event(json::array({text("INVALID_JUDGE_SILENT")})));
        require(services.judges().size() == ++judges && services.chats().size() == chats, "无效判定被重试或调用聊天兜底");
        CHAT_LLM_REPLY_PROBABILITY = 1;
        deliver(bot, event(json::array({text("INVALID_JUDGE_PROBABILITY")})));
        require(services.judges().size() == ++judges && services.chats().size() == ++chats, "无效判定未进入概率路径");
    }

    services.judge("NO", false, 1500);
    CHAT_LLM_REPLY_PROBABILITY = 0;
    deliver(bot, event(json::array({text("TIMEOUT_JUDGE_SILENT")})));
    require(services.judges().size() == ++judges && services.chats().size() == chats, "判定超时重试或触发聊天");
    CHAT_LLM_REPLY_PROBABILITY = 1;
    deliver(bot, event(json::array({text("TIMEOUT_JUDGE_PROBABILITY")})));
    require(services.judges().size() == ++judges && services.chats().size() == ++chats, "判定超时概率命中未触发聊天");
    services.judge("NO");
    const auto judge_host = JUDGE_HOST;
    JUDGE_HOST = "http://127.0.0.1:1";
    CHAT_LLM_REPLY_PROBABILITY = 0;
    deliver(bot, event(json::array({text("NETWORK_JUDGE_SILENT")})));
    require(services.chats().size() == chats, "判定网络失败概率0触发聊天");
    CHAT_LLM_REPLY_PROBABILITY = 1;
    deliver(bot, event(json::array({text("NETWORK_JUDGE_PROBABILITY")})));
    require(services.chats().size() == ++chats, "判定网络失败概率1未触发聊天");
    deliver(bot, event(json::array({at("42"), text("DIRECT_WITH_JUDGE_DOWN")})));
    require(services.chats().size() == ++chats && services.judges().size() == judges, "判定不可用阻塞直接唤醒");
    const auto referenced_bot = services.lastMessageId();
    for (const auto& segments : {json::array({at(42)}), json::array({reply(referenced_bot)}),
            json::array({at("42"), {{"type", "record"}, {"data", {{"file", "DIRECT_VOICE"}}}}})})
    {
        const auto direct_empty = event(segments);
        deliver(bot, direct_empty);
        require(services.chats().size() == ++chats && services.judges().size() == judges,
            "判定不可用时空@、空引用或无正文媒体未直接聊天");
        replyForm(services.sends().back(), segments.front().at("type") == "at" ? direct_empty.at("message_id").dump() : "");
    }
    JUDGE_HOST = judge_host;
    CHAT_LLM_REPLY_PROBABILITY = 0;

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
    CHAT_LLM_REPLY_PROBABILITY = 0;
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
    CHAT_LLM_REPLY_PROBABILITY = 1;
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
    CHAT_LLM_REPLY_PROBABILITY = 0;

    const std::string historical_url = "https://images.example/member-a.png";
    auto historical = event(json::array({image(historical_url)}), 815, 88);
    historical["message"][0]["data"]["summary"] = historical_url;
    historical["message"][0]["data"]["title"] = historical_url;
    deliver(bot, historical);
    deliver(bot, event(json::array({image("https://images.example/other-group.png")}), 816, 99));
    current = event(json::array({at("42"), text("ASK_MEMBER_A_IMAGE")}), 815, 77);
    deliver(bot, current);
    const auto history = services.chats().back().at("messages");
    require(imageUrls(history).empty() && history.dump().find(historical_url) == std::string::npos, "未引用历史图片仍带原图或旁路URL");
    require(occurrences(history.dump(), "ASK_MEMBER_A_IMAGE") == 1, "当前图片追问进入请求多次");
    bool attributed = false;
    for (const auto& message : history)
    {
        const auto record = modelContent(message);
        if (record.is_object() && record.value("message_id", "") == historical.at("message_id").dump())
            attributed = record.at("user_id") == "88" && imageUrls(json::array({message})).empty() && record.at("media").size() == 1;
    }
    require(attributed, "历史图片媒体标记未与原发言人及消息对应");
    const auto referenced = event(json::array({at("42"), reply(historical.at("message_id").dump()), text("EXPLICIT_MEMBER_IMAGE_QUERY")}), 815, 77);
    deliver(bot, referenced);
    const auto explicit_images = services.chats().back().at("messages");
    require(imageUrls(explicit_images) == json::array({historical_url}), "直接引用成员图片未附原图");
    require(occurrences(explicit_images.dump(), historical.at("message_id").dump()) >= 1, "直接引用失去原消息ID");
    size_t referenced_copies = 0;
    for (const auto& message : explicit_images)
    {
        const auto record = modelContent(message);
        if (record.is_object() && record.value("message_id", "") == historical.at("message_id").dump()) ++referenced_copies;
    }
    require(referenced_copies == 1, "直接引用与近期窗口重复注入原消息");
    deliver(bot, event(json::array({at("42"), reply(historical.at("message_id").dump()),
        image("https://images.example/current-with-reference.png"), text("BOTH_IMAGE_SOURCES")}), 815));
    require(imageUrls(services.chats().back().at("messages")) == json::array({historical_url, "https://images.example/current-with-reference.png"}),
        "当前图与直接引用原图未同时保留或混入其他图片");
    const auto before_member_judges = services.judges().size();
    const auto before_member_chats = services.chats().size();
    deliver(bot, event(json::array({reply(historical.at("message_id").dump()), text("MEMBER_IMAGE_NOT_DIRECT_WAKE")}), 815));
    require(services.judges().size() == before_member_judges + 1 && services.chats().size() == before_member_chats,
        "引用其他成员图片改变了判定和概率唤醒路径");
    CHAT_LLM_MAX_MESSAGES = 1;
    deliver(bot, event(json::array({at("42"), text("CROPPED_IMAGE_QUERY")}), 815));
    require(imageUrls(services.chats().back().at("messages")).empty(), "裁剪历史消息后仍发送所属图片");
    CHAT_LLM_MAX_MESSAGES = 120;

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

    const auto original_budget = CHAT_LLM_CONTEXT_TOKENS;
    CHAT_LLM_CONTEXT_TOKENS = 3000;
    deliver(bot, event(json::array({image("https://images.example/" + std::string(10000, 'U'))}), 821));
    deliver(bot, event(json::array({at("42"), text("URL_BUDGET_CURRENT")}), 821));
    const auto bounded = services.chats().back().at("messages");
    require(estimatedTokens(bounded) + CHAT_LLM_MAX_TOKENS <= CHAT_LLM_CONTEXT_TOKENS && imageUrls(bounded).empty(), "纯文字追问预算失效或仍附历史原图");
    require(bounded.dump().find("URL_BUDGET_CURRENT") != std::string::npos, "图片URL预算裁剪丢失当前问题");
    chats = services.chats().size();
    sends = services.sends().size();
    current = event(json::array({at("42"), image("https://images.example/" + std::string(10000, 'V'))}), 823);
    deliver(bot, current);
    require(services.chats().size() == chats && services.sends().size() == sends + 1, "当前图片视觉开销超过预算仍发请求或未提示失败");
    replyForm(services.sends().back(), current.at("message_id").dump());
    require(services.sends().back().dump().find("AFTER_IMAGE_FAILURE") == std::string::npos, "图片预算不足伪造聊天成功");
    require(services.sends().back().dump().find("预算") != std::string::npos, "视觉预算不足缺少明确提示");
    CHAT_LLM_CONTEXT_TOKENS = 6000;
    current = event(json::array({at("42"), image("https://images.example/" + std::string(10000, 'W'))}), 825);
    deliver(bot, current);
    require(services.chats().size() == chats + 1 && imageUrls(services.chats().back().at("messages")).size() == 1,
        "图片URL字节长度被错误代替视觉token开销");
    CHAT_LLM_CONTEXT_TOKENS = original_budget;

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
    deliver(bot, event(json::array({at("42"), reply(image_receipts.front().message_id), text("BOT_IMAGE_HISTORY_QUERY")}), 824));
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
        const auto timeline = readTimeline(root / "data/AiModule");
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
            return readTimeline(root / "data/AiModule").dump().find("https://images.example/concurrent.png") != std::string::npos;
        }, "图片未在生成期间进入群时间线");
        collected = true;
    }
    catch (const std::runtime_error&) {}
    services.unblockChat();
    pending.get();
    incoming.get();
    require(collected, "图片未在生成期间进入群时间线");
    deliver(bot, event(json::array({at("42"), text("IMAGE_CONCURRENT_CHECK")}), 822));
    require(imageUrls(services.chats().back().at("messages")).empty() && services.chats().back().at("messages").dump().find("image") != std::string::npos,
        "生成期间收到图片未保留媒体标记或未引用仍重复附图");
}

static void temporalBoundaries(Services& services)
{
    ChatTaskManager manager;
    services.chat("TEMPORAL_REPLY");
    const auto future_event = event(json::array({image("https://images.example/future.png"), text("AFTER_CURRENT_EVENT_MARKER")}), 1701);
    auto first = MessageManager::getMessageContext(event(json::array({at("42"), reply(future_event.at("message_id").dump()), text("CUTOFF_CURRENT_MARKER")}), 1701));
    auto future = MessageManager::getMessageContext(future_event);
    manager.recordIncoming(first);
    manager.recordIncoming(future);
    manager.handleTask(first);
    auto messages = services.chats().back().at("messages");
    require(messages.dump().find("AFTER_CURRENT_EVENT_MARKER") == std::string::npos && imageUrls(messages).empty() &&
        messages.dump().find("unavailable") != std::string::npos, "本次事件之后的消息通过近期窗口或直接引用进入请求");
    auto expired_event = event(json::array({image("https://images.example/expired-record.png"), text("EXPIRED_HISTORY_MARKER")}), 1701);
    expired_event["time"] = static_cast<int64_t>(std::time(nullptr)) - 31 * 86400;
    auto expired = MessageManager::getMessageContext(expired_event);
    manager.recordIncoming(expired);
    auto current = MessageManager::getMessageContext(event(json::array({at("42"), reply(expired.message_id), text("EXPIRED_REFERENCE_CURRENT")}), 1701));
    manager.recordIncoming(current);
    manager.handleTask(current);
    messages = services.chats().back().at("messages");
    require(messages.dump().find("EXPIRED_HISTORY_MARKER") == std::string::npos && imageUrls(messages).empty() &&
        messages.dump().find("unavailable") != std::string::npos, "过期消息通过近期窗口或直接引用进入请求");
    require(manager.shutdown(), "时间边界测试最终归档失败");
}

static void protectedInputs(Services& services)
{
    ChatTaskManager manager;
    const auto saved_budget = CHAT_LLM_CONTEXT_TOKENS;
    const auto saved_prompt = CHAT_LLM_SYS_PROMPTS;
    CHAT_LLM_CONTEXT_TOKENS = 4000;
    const auto original = event(json::array({text("PROTECTED_REFERENCE_" + std::string(4000, 'R'))}), 1702, 88);
    auto reference = MessageManager::getMessageContext(original);
    manager.recordIncoming(reference);
    auto noise = MessageManager::getMessageContext(event(json::array({text(std::string(20000, 'N'))}), 1702, 89));
    manager.recordIncoming(noise);
    auto current = MessageManager::getMessageContext(event(json::array({at("42"), reply(reference.message_id), text("PROTECTED_CURRENT_" + std::string(500, 'C'))}), 1702));
    manager.recordIncoming(current);
    const auto before = services.chats().size();
    manager.handleTask(current);
    require(services.chats().size() == before + 1, "可容纳完整保护输入时未调用聊天模型");
    const auto messages = services.chats().back().at("messages");
    bool full_reference = false;
    bool full_current = false;
    for (const auto& message : messages)
    {
        const auto record = modelContent(message);
        if (!record.is_object()) continue;
        if (record.value("message_id", "") == reference.message_id) full_reference = record.at("text") == reference.pmsgsegs.text;
        if (record.value("message_id", "") == current.message_id) full_current = record.at("text") == current.pmsgsegs.text;
    }
    require(full_reference && full_current && messages.dump().find(std::string(10000, 'N')) == std::string::npos,
        "预算裁剪截短当前问题或直接引用，或保留超长普通历史");
    require(estimatedTokens(messages) + CHAT_LLM_MAX_TOKENS <= CHAT_LLM_CONTEXT_TOKENS, "保护输入请求超过token预算");
    CHAT_LLM_SYS_PROMPTS = std::string(20000, 'P');
    const auto failed = manager.handleTask(current);
    require(services.chats().size() == before + 1 && failed.first.dump().find("预算") != std::string::npos,
        "静态提示词超预算被截短或缺少明确提示");
    CHAT_LLM_CONTEXT_TOKENS = saved_budget;
    CHAT_LLM_SYS_PROMPTS = saved_prompt;
    require(manager.shutdown(), "保护输入测试最终归档失败");
}

static void archivedReferences(Services& services, const std::filesystem::path& root)
{
    CHAT_LLM_MAX_MESSAGES = 2;
    JUDGE_MAX_MESSAGES = 1;
    CHAT_LLM_REPLY_PROBABILITY = 0;
    services.judge("NO");
    services.chat("ARCHIVED_BOT_REPLY");
    const auto ancestor = event(json::array({image("https://images.example/no-recursive-image.png")}), 1601, 88);
    const auto original = event(json::array({reply(ancestor.at("message_id").dump()), image("https://images.example/db-reference.png"), text("ARCHIVED_IMAGE_SOURCE")}), 1601, 89);
    std::string bot_reply;
    {
        BotServer bot;
        deliver(bot, ancestor);
        deliver(bot, original);
        deliver(bot, event(json::array({at("42"), text("ARCHIVED_BOT_INPUT")}), 1601));
        bot_reply = services.lastMessageId();
        for (int i = 0; i < 6; ++i)
            deliver(bot, event(json::array({{{"type", "record"}, {"data", {{"file", "voice"}}}}}), 1601, 90));
        require(bot.tsk_manager.shutdown(), "DB引用样本最终保存失败");
    }
    {
        BotServer bot;
        deliver(bot, event(json::array({at("42"), reply(original.at("message_id").dump()), text("RESTORE_EXPLICIT_IMAGE")}), 1601));
        const auto messages = services.chats().back().at("messages");
        require(imageUrls(messages) == json::array({"https://images.example/db-reference.png"}),
            "DB直接引用未恢复原图或递归附带引用链图片");
        size_t sources = 0;
        for (const auto& message : messages)
        {
            const auto record = modelContent(message);
            if (record.is_object() && record.value("message_id", "") == original.at("message_id").dump())
            {
                ++sources;
                require(record.at("user_id") == "89" && record.at("timestamp") == original.at("time") &&
                    record.at("reply_to") == ancestor.at("message_id").dump(), "DB引用未保留发送者、时间或引用关系");
            }
        }
        require(sources == 1, "DB直接引用重复进入请求");
        const auto judges = services.judges().size();
        const auto chats = services.chats().size();
        deliver(bot, event(json::array({reply(bot_reply), text("ARCHIVED_BOT_DIRECT_WAKE")}), 1601));
        require(services.chats().size() == chats + 1 && services.judges().size() == judges,
            "DB确认的机器人回复未沿用直接唤醒");
        for (const auto& missing : {original.at("message_id").dump(), std::string("never-existed")})
        {
            deliver(bot, event(json::array({at("42"), reply(missing), text("MISSING_REFERENCE_QUERY")}), 1602));
            const auto absent = services.chats().back().at("messages");
            require(imageUrls(absent).empty() && absent.dump().find("unavailable") != std::string::npos,
                "跨群或缺失引用未保留不可用标记，或混入其他群原图");
        }
        require(bot.tsk_manager.shutdown(), "DB引用测试停止保存失败");
    }
    require(readTimeline(root / "data/AiModule").at("1601").size() > CHAT_LLM_MAX_MESSAGES,
        "内存窗口淘汰删除了有效归档");
    CHAT_LLM_MAX_MESSAGES = 120;
    JUDGE_MAX_MESSAGES = 12;
}

static void pokeActions(Services& services, const std::filesystem::path& root)
{
    BotServer bot;
    CHAT_LLM_REPLY_PROBABILITY = 0;
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
    first["time"] = static_cast<int64_t>(std::time(nullptr));
    deliver(bot, first);
    auto second = poke(905, 88);
    second["time"] = static_cast<int64_t>(std::time(nullptr));
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
        else if (record.at("user_id") == "77" && record.at("timestamp") == first.at("time")) ++first_seen;
        else if (record.at("user_id") == "88" && record.at("timestamp") == second.at("time")) ++second_seen;
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

    const auto timeline_path = root / "data/AiModule";
    eventually([&] {
        const auto timelines = readTimeline(timeline_path);
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
    CHAT_LLM_REPLY_PROBABILITY = 0;
    CHAT_LLM_MAX_MESSAGES = 4;
    JUDGE_MAX_MESSAGES = 2;
    for (int i = 0; i < 8; ++i)
        deliver(bot, event(json::array({text("WINDOW_HISTORY_" + std::to_string(i))}), 321));
    deliver(bot, event(json::array({at("42"), text("WINDOW_CURRENT_MARKER")}), 321));
    const auto window_messages = services.chats().back().at("messages");
    std::string messages = window_messages.dump();
    std::string recent;
    for (const auto& message : window_messages)
    {
        const auto record = modelContent(message);
        if (record.is_object() && record.contains("sequence")) recent += record.dump();
    }
    require(recent.find("WINDOW_HISTORY_0") == std::string::npos && recent.find("WINDOW_HISTORY_7") != std::string::npos, "聊天窗口未裁剪最早历史");
    deliver(bot, event(json::array({text("JUDGE_WINDOW_CURRENT")}), 321));
    messages = services.judges().back().at("messages").dump();
    require(messages.find("WINDOW_HISTORY_6") == std::string::npos && occurrences(messages, "JUDGE_WINDOW_CURRENT") == 1, "判定未使用较短同源窗口");

    CHAT_LLM_CONTEXT_TOKENS = 1800;
    JUDGE_CONTEXT_TOKENS = 1600;
    CHAT_LLM_SYS_PROMPTS = std::string(5000, 'P');
    auto chats = services.chats().size();
    auto judges = services.judges().size();
    auto sends = services.sends().size();
    deliver(bot, event(json::array({at("42"), text("BUDGET_CURRENT_" + std::string(6000, 'X'))}), 321));
    require(services.chats().size() == chats && services.sends().size() == sends + 1,
        "必需输入超预算仍请求模型或未给直接唤醒提示");
    require(services.sends().back().dump().find("预算") != std::string::npos, "直接唤醒缺少明确超预算提示");
    deliver(bot, event(json::array({text("JUDGE_BUDGET_CURRENT_" + std::string(6000, 'Y'))}), 321));
    require(services.judges().size() == judges && services.chats().size() == chats && services.sends().size() == sends + 1,
        "判定超预算仍请求模型或普通路径发送错误提示");
    CHAT_LLM_SYS_PROMPTS = "SYSTEM_PERSONA_MARKER";
    CHAT_LLM_CONTEXT_TOKENS = 32000;
    JUDGE_CONTEXT_TOKENS = 1;
    CHAT_LLM_REPLY_PROBABILITY = 1;
    deliver(bot, event(json::array({text("JUDGE_BUDGET_PROBABILITY_FALLBACK")}), 322));
    require(services.judges().size() == judges && services.chats().size() == chats + 1,
        "判定预算失败未沿用概率路径");
    require(estimatedTokens(services.chats().back().at("messages")) + CHAT_LLM_MAX_TOKENS <= CHAT_LLM_CONTEXT_TOKENS,
        "概率回退的聊天请求超过自身token预算");
    CHAT_LLM_REPLY_PROBABILITY = 0;

    CHAT_LLM_CONTEXT_TOKENS = 32000;
    JUDGE_CONTEXT_TOKENS = 4000;
    CHAT_LLM_SYS_PROMPTS = "SYSTEM_PERSONA_MARKER";
    CHAT_LLM_MAX_MESSAGES = 120;
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

static void persistenceWithoutChatLlm(Services& services, const std::filesystem::path& root)
{
    const auto timeline = root / "data/AiModule";
    const auto judges = services.judges().size();
    const auto chats = services.chats().size();
    services.judge("NO");
    CHAT_LLM_REPLY_PROBABILITY = 0;
    {
        BotServer bot;
        deliver(bot, event(json::array({text("PERSIST_WITHOUT_AI_MARKER")}), 654, 81));
        require(services.judges().size() == judges + 1 && services.chats().size() == chats, "持久化样本意外触发聊天");
        eventually([&] { return std::filesystem::exists(timeline / "messages.sqlite3") && readTimeline(timeline).dump().find("PERSIST_WITHOUT_AI_MARKER") != std::string::npos; }, "未触发AI的普通消息未定时保存");
    }
    {
        BotServer bot;
        services.chat("RESTORED_REPLY");
        deliver(bot, event(json::array({at("42"), text("RESTORE_CHECK_MARKER")}), 654, 82));
        const std::string messages = services.chats().back().at("messages").dump();
        require(messages.find("PERSIST_WITHOUT_AI_MARKER") != std::string::npos, "重启未恢复已经保存的普通群历史");
        require(messages.find("LEGACY_HISTORY_MARKER") == std::string::npos, "旧个人历史被伪造为群时间线");
        deliver(bot, event(json::array({at("42"), text("RESTORED_IMAGE_QUERY")}), 822));
        require(imageUrls(services.chats().back().at("messages")).empty() && services.chats().back().at("messages").dump().find("image") != std::string::npos,
            "重启丢失历史图片媒体标记或无引用仍附图");
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

static void removedSummaries(Services& services, const std::filesystem::path& root)
{
    const auto data = root / "data/AiModule";
    const std::vector<std::string> files = {"persona_map.json", "users_profile_map.json", "session_round_counter.json", "group_round_counter.json"};
    for (const auto& filename : files)
    {
        std::ofstream output(data / filename);
        output << "{damaged-legacy-json";
    }
    services.chat("STATIC_PROMPT_REPLY");
    const auto before = services.chats().size();
    {
        BotServer bot;
        for (int i = 0; i < 35; ++i)
            deliver(bot, event(json::array({at("42"), text("STATIC_PROMPT_INPUT_" + std::to_string(i))}), 777, 7));
        require(bot.tsk_manager.shutdown(), "正常停止未完成最终入库");
    }
    const auto calls = services.chats();
    require(calls.size() == before + 35, "连续聊天产生额外总结调用");
    for (size_t i = before; i < calls.size(); ++i)
    {
        const auto request = calls[i].at("messages").dump();
        require(request.find("SYSTEM_PERSONA_MARKER") != std::string::npos, "静态提示词被删除");
        require(request.find("GROUP_123_PERSONA_MARKER") == std::string::npos && request.find("GROUP_123_PROFILE_MARKER") == std::string::npos,
            "停用的画像或人格进入请求");
        require(request.find("总结器") == std::string::npos, "仍在调用总结模型");
    }
    for (const auto& filename : files)
    {
        std::ifstream input(data / filename);
        const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(saved == "{damaged-legacy-json", "停用的旧文件被写入");
    }
}

static void streamingLifecycle(Services& services)
{
    BotServer bot;
    const auto judges = services.judges().size();
    const auto timeout = CHAT_LLM_TIMEOUT_SECONDS;
    services.chat("DEFAULT_STREAM_REPLY");
    const std::string fragmented = ": heartbeat\r\n\r\nevent: message\r\n"
        "data: {\"choices\":\r\ndata: [{\"index\":0,\"delta\":{\"content\":\"中文正文\"}}]}\r\n\r\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"HIDDEN_STREAM_REASONING\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"尾声\"}}]}\r\r"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\r\n\r\n"
        "data: {\"choices\":[],\"usage\":{\"total_tokens\":123}}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    services.stream(fragmented, 1);
    size_t sends = services.sends().size();
    size_t chats = services.chats().size();
    deliver(bot, event(json::array({at("42"), text("FRAGMENTED_STREAM_INPUT")}), 1101));
    require(services.chats().size() == ++chats && services.sends().size() == sends + 1 &&
        services.sends().back().at("message").back().at("data").at("text") == "中文正文尾声",
        "跨字节分片、多行data或不同换行的SSE未按正文顺序聚合");

    services.stream();
    for (bool action : {false, true})
    {
        services.chat(action ? json{{"text", "COMPLETE_POKE_BEFORE_DONE"}, {"poke", true}}.dump() : "COMPLETE_TEXT_BEFORE_DONE");
        services.pauseStream();
        sends = services.sends().size();
        const auto pokes = services.pokes().size();
        auto delivery = std::async(std::launch::async, [&] {
            deliver(bot, action ? poke(1102) : event(json::array({at("42"), text("WAIT_FOR_STREAM_DONE")}), 1102));
        });
        services.awaitStream();
        const bool no_early_output = services.sends().size() == sends && services.pokes().size() == pokes &&
            delivery.wait_for(0ms) == std::future_status::timeout;
        services.unblockStream();
        delivery.get();
        require(no_early_output, "正文和拍决策在流完整结束前产生出站行为");
        require(services.sends().size() == sends + 1 && services.pokes().size() == pokes + (action ? 1 : 0),
            "完整流结束后没有按聚合结果发送文字或回拍");
        ++chats;
    }

    std::string incomplete = streamReply("HALF_STREAM_PRIVATE_MARKER");
    incomplete.erase(incomplete.find("data: [DONE]"));
    for (int fault = 0; fault < 5; ++fault)
    {
        const std::string data = fault == 3 ? incomplete + "data: {bad-json}\r\n\r\ndata: [DONE]\r\n\r\n" :
            fault == 4 ? incomplete + "data: [DONE]" : incomplete;
        services.stream(data, 0, fault == 1, fault == 2 ? 1200 : 0);
        CHAT_LLM_TIMEOUT_SECONDS = fault == 2 ? 1 : timeout;
        services.judge("YES");
        for (int kind = 0; kind < 3; ++kind)
        {
            sends = services.sends().size();
            const auto pokes = services.pokes().size();
            deliver(bot, kind == 2 ? poke(1103) : event(kind == 0 ? json::array({at("42"), text("FAILED_STREAM_DIRECT")}) :
                json::array({text("FAILED_STREAM_ORDINARY")}), 1103));
            require(services.chats().size() == ++chats && services.sends().size() == sends + (kind == 1 ? 0 : 1) &&
                services.pokes().size() == pokes, "流失败产生半截结果、重复请求或回拍");
            if (kind != 1)
                require(services.sends().back().at("message").back().at("data").at("text") == "暂时无法获取回复，请稍后再试。",
                    "流失败未沿用直接唤醒失败提示");
        }
    }
    CHAT_LLM_TIMEOUT_SECONDS = 1;
    const auto slow_stream = streamReply("TOTAL_TIMEOUT_PRIVATE_MARKER");
    services.stream(slow_stream, slow_stream.size() / 8, false, 0, 200);
    sends = services.sends().size();
    deliver(bot, event(json::array({at("42"), text("CONTINUOUS_STREAM_TOTAL_TIMEOUT")}), 1103));
    require(services.chats().size() == ++chats && services.sends().size() == sends + 1 &&
        services.sends().back().at("message").back().at("data").at("text") == "暂时无法获取回复，请稍后再试。",
        "持续收到分片时请求总超时未生效");
    CHAT_LLM_TIMEOUT_SECONDS = timeout;
    services.stream();
    services.judge("NO");
    services.chat("AFTER_INCOMPLETE_STREAM");
    deliver(bot, event(json::array({at("42"), text("CHECK_INCOMPLETE_STREAM_HISTORY")}), 1103));
    const auto after_failure = services.chats().back().at("messages").dump();
    require(after_failure.find("HALF_STREAM_PRIVATE_MARKER") == std::string::npos &&
        after_failure.find("TOTAL_TIMEOUT_PRIVATE_MARKER") == std::string::npos,
        "失败流的半截正文被记录为成功出站历史");
    require(services.judges().size() == judges + 5, "流式直接唤醒错误调用判定或判定被重试");
}

static void legacyEmptyAndDamaged(const std::filesystem::path& root)
{
    const std::string original_data = DATA_PATH;
    DATA_PATH = (root / "empty-data").string();
    const auto data = std::filesystem::path(DATA_PATH) / "AiModule";
    std::filesystem::create_directories(data);
    for (const auto& filename : {"persona_map.json", "users_profile_map.json", "session_round_counter.json", "group_round_counter.json"})
    {
        std::ofstream output(data / filename);
        output << "{damaged-json";
    }
    {
        BotServer bot;
        deliver(bot, event(json::array({text("DAMAGED_IGNORED_MESSAGE_MARKER")})));
        require(bot.tsk_manager.shutdown(), "正常停止保存失败");
    }
    require(readTimeline(data).dump().find("DAMAGED_IGNORED_MESSAGE_MARKER") != std::string::npos,
        "已停用旧文件损坏仍阻止新消息保存");
    {
        BotServer bot;
        require(bot.tsk_manager.shutdown(), "重启后正常停止失败");
    }
    for (const auto& filename : {"persona_map.json", "users_profile_map.json", "session_round_counter.json", "group_round_counter.json"})
    {
        std::ifstream input(data / filename);
        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(content == "{damaged-json", "已停用旧文件被覆盖");
    }
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
        commandRouting(services, root);
        std::cout << "命令入口、参数、业务结果和成功交付记录通过\n";
        routingAndHistory(services);
        std::cout << "分流、共享上下文、发送回执通过\n";
        multimodalAndReplies(services, root);
        std::cout << "图片多模态、来源隔离、回复形式通过\n";
        temporalBoundaries(services);
        protectedInputs(services);
        archivedReferences(services, root);
        pokeActions(services, root);
        std::cout << "拍事件、动作约束、部分失败记录通过\n";
        windowBudgetAndConcurrency(services);
        std::cout << "窗口、输入预算、并发收集通过\n";
        persistenceWithoutChatLlm(services, root);
        streamingLifecycle(services);
        std::cout << "流式分片、完整性、失败及延迟出站通过\n";
        removedSummaries(services, root);
        legacyEmptyAndDamaged(root);
        require(readJson(data / "session_memory.json") == legacy, "旧个人历史文件被修改");
        std::cout << "普通消息定时保存、重启恢复、旧文件保留通过\n";
        std::cout << "画像与人格移除、静态提示词和旧文件保留通过\n";
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
