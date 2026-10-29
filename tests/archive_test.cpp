#include "ToolManagers/ChatArchive.h"
#include "ToolManagers/MessageManager.h"
#include "config.h"
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message)
{
    if(!condition) throw std::runtime_error(message);
}

template<class Predicate>
void eventually(Predicate predicate, const std::string& message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error(message);
}

json record(uint64_t sequence, const std::string& group, const std::string& text, int64_t timestamp = std::time(nullptr))
{
    return {{"sequence", sequence}, {"group_id", group}, {"message_id", "message-" + std::to_string(sequence)},
            {"user_id", "member-" + group}, {"nickname", "成员"}, {"card", "群名片"}, {"timestamp", timestamp},
            {"text", text}, {"segments", json::array({{{"type", "text"}, {"data", {{"text", text}}}}})},
            {"reply_to", ""}, {"reply_to_bot", false}, {"bot", false}, {"event_type", "message"}, {"target_id", ""}};
}

void write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << text;
    require(bool(output), "测试文件写入失败");
}

std::string read(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class Database {
    sqlite3* db = nullptr;

public:
    explicit Database(const std::filesystem::path& directory)
    {
        require(sqlite3_open((directory / "messages.sqlite3").c_str(), &db) == SQLITE_OK, "测试数据库连接失败");
        sqlite3_busy_timeout(db, 2000);
    }
    ~Database() { sqlite3_close(db); }
    void run(const std::string& sql)
    {
        char* error = nullptr;
        const auto result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
        const std::string reason = error ? error : "";
        sqlite3_free(error);
        require(result == SQLITE_OK, "测试SQL执行失败: " + reason);
    }
    int64_t number(const std::string& sql)
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "测试SQL准备失败");
        const auto result = sqlite3_step(statement);
        const auto value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        require(result == SQLITE_ROW, "测试SQL没有结果");
        return value;
    }
};

class Embeddings {
    httplib::Server server;
    std::thread worker;

public:
    int port = 0;
    std::atomic<int> requests{0};
    std::atomic<bool> fail{false}, slow{false}, invalid{false}, leaked_image{false};

    Embeddings()
    {
        server.Post("/embeddings", [this](const httplib::Request& request, httplib::Response& response) {
            ++requests;
            const auto body = json::parse(request.body);
            require(body["model"] == EMBEDDING_MODEL && body["encoding_format"] == "float", "embedding请求协议错误");
            require(request.get_header_value("Authorization") == "Bearer " + EMBEDDING_KEY, "embedding密钥请求错误");
            const auto text = body["input"].get<std::string>();
            if(text.find("https://archive.example/image.png") != std::string::npos) leaked_image = true;
            if(slow) std::this_thread::sleep_for(std::chrono::milliseconds(1800));
            if(fail)
            {
                response.status = 503;
                response.set_content("{\"error\":\"embedding unavailable\"}", "application/json");
                return;
            }
            json vector = json::array({0.0, 0.0, 1.0});
            if(text.find("火星") != std::string::npos || text.find("红色星球") != std::string::npos) vector = json::array({1.0, 0.0, 0.0});
            else if(text.find("烹饪") != std::string::npos || text.find("厨房") != std::string::npos) vector = json::array({0.0, 1.0, 0.0});
            if(invalid) vector = json::array({0.0, 0.0});
            response.set_content(json{{"data", json::array({{{"index", 0}, {"embedding", vector}}})}}.dump(), "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        require(port > 0, "embedding模拟服务绑定失败");
        worker = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~Embeddings()
    {
        server.stop();
        if(worker.joinable()) worker.join();
    }
};

void configure(Embeddings& service, const std::filesystem::path& root)
{
    LOG_FILE_PATH = (root / "archive.log").string();
    LOG_CONSOLE_LEVEL = LOG_FILE_LEVEL = 3;
    EMBEDDING_HOST = "http://127.0.0.1";
    EMBEDDING_PORT = service.port;
    EMBEDDING_POST_PATH = "/embeddings";
    EMBEDDING_MODEL = "archive-test";
    EMBEDDING_KEY = "archive-key";
    EMBEDDING_TIMEOUT_SECONDS = 1;
}

void migration(const std::filesystem::path& directory)
{
    const auto now = int64_t(std::time(nullptr));
    auto original = record(10, "123", "迁移来源信息", now - 30);
    original["bot"] = true;
    original["user_id"] = "42";
    original["reply_to"] = "prior";
    original["segments"].push_back({{"type", "image"}, {"data", {{"url", "https://archive.example/image.png"}, {"file", "base64://DO_NOT_ARCHIVE"}}}});
    original["segments"].push_back({{"type", "json"}, {"data", {{"data", json{{"prompt", "卡片提示"},
        {"meta", {{"detail", {{"title", "卡片标题"}, {"desc", "卡片说明"}, {"preview", "base64://DO_NOT_ARCHIVE"}}}}},
        {"image", "base64://DO_NOT_ARCHIVE"}}.dump()}}}});
    auto duplicate = record(11, "123", "同消息ID再次上报", now - 20);
    duplicate["message_id"] = "message-10";
    const auto expired = record(100, "123", "过期消息", now - 31 * 24 * 3600);
    const auto legacy = json{{"group_timeline", {{"123", json::array({original, duplicate, expired})}}}}.dump();
    write(directory / "group_timeline.json", legacy);
    for(const auto* name : {"persona_map.json", "users_profile_map.json", "session_round_counter.json", "group_round_counter.json", "session_memory.json"})
        write(directory / name, "damaged legacy data");
    {
        ChatArchive archive(directory.string());
        require(archive.lastSequence() == 100, "过期迁移消息未推进最高序号");
        const auto recent = archive.recent(120);
        require(recent["123"].size() == 2 && recent["123"][0]["sequence"] == 10, "迁移保留期或重复ID保留错误");
        const auto restored = archive.find("123", "message-10", 11, now);
        require(restored["bot"] == true && restored["user_id"] == "42" && restored["reply_to"] == "prior", "迁移丢失来源或引用");
        require(restored.dump().find("DO_NOT_ARCHIVE") == std::string::npos, "归档保存了图片Base64");
        require(restored.dump().find("https://archive.example/image.png") != std::string::npos, "归档丢失原图URL");
        require(restored.dump().find("卡片提示") != std::string::npos && restored.dump().find("卡片标题") != std::string::npos &&
                restored.dump().find("卡片说明") != std::string::npos, "归档丢失卡片可用文本");
        require(archive.find("other", "message-10", 101, now).is_null(), "引用跨群读取");
        require(archive.find("123", "message-10", 10, now).is_null(), "引用读到本次序号");
        require(archive.find("123", "message-10", 101, now - 31).is_null(), "引用读到事件之后的消息");
        require(archive.find("123", "message-10", 101, now)["sequence"] == 11, "重复消息ID引用未选择截止前最新记录");
        require(!archive.find("123", "message-10", std::numeric_limits<uint64_t>::max(), now).is_null(), "引用大before溢出");
    }
    require(read(directory / "group_timeline.json") == legacy, "迁移改写了旧时间线");
    write(directory / "group_timeline.json", "damaged after migration");
    {
        ChatArchive archive(directory.string());
        require(archive.recent(120)["123"].size() == 2 && archive.lastSequence() == 100, "重启重复迁移或读取停用文件");
        require(archive.find("123", "message-10", 11, now).dump().find("卡片说明") != std::string::npos, "重启丢失卡片可用文本");
    }
    for(const auto* name : {"persona_map.json", "users_profile_map.json", "session_round_counter.json", "group_round_counter.json", "session_memory.json"})
        require(read(directory / name) == "damaged legacy data", "停用文件被修改");
}

void damaged(const std::filesystem::path& root)
{
    const auto old = root / "damaged-json";
    write(old / "group_timeline.json", "{broken");
    bool rejected = false;
    try { ChatArchive archive(old.string()); } catch(const std::exception&) { rejected = true; }
    require(rejected && read(old / "group_timeline.json") == "{broken", "损坏时间线被空状态覆盖");
    write(old / "group_timeline.json", json{{"group_timeline", {{"1", json::array({record(1, "1", "重试迁移")})}}}}.dump());
    { ChatArchive archive(old.string()); require(archive.recent(1)["1"].size() == 1, "失败迁移错误标记完成"); }
    for(const auto& bytes : {std::string("not a sqlite database"), std::string()})
    {
        const auto path = root / (bytes.empty() ? "empty-db" : "damaged-db");
        write(path / "messages.sqlite3", bytes);
        rejected = false;
        try { ChatArchive archive(path.string()); } catch(const std::exception&) { rejected = true; }
        require(rejected && read(path / "messages.sqlite3") == bytes, "损坏数据库未拒绝或被改写");
    }
    const auto semantic = root / "damaged-record";
    { ChatArchive archive(semantic.string()); archive.append(json::array({record(1, "1", "合法消息")})); }
    { Database database(semantic); database.run("UPDATE messages SET record='{}'"); }
    rejected = false;
    try { ChatArchive archive(semantic.string()); } catch(const std::exception&) { rejected = true; }
    require(rejected, "数据库内损坏记录未拒绝启动");
}

void transactions(const std::filesystem::path& directory)
{
    ChatArchive archive(directory.string());
    archive.append(json::array({record(1, "group", "已提交")}));
    Database database(directory);
    database.run("CREATE TRIGGER fail_batch BEFORE INSERT ON messages WHEN NEW.sequence=3 BEGIN SELECT RAISE(ABORT,'write failure'); END");
    bool rejected = false;
    const auto batch = json::array({record(2, "group", "批次前半"), record(3, "group", "批次后半")});
    try { archive.append(batch); } catch(const std::exception&) { rejected = true; }
    require(rejected && archive.lastSequence() == 1 && archive.recent(120)["group"].size() == 1, "失败事务留下半批或改写最高序号");
    database.run("DROP TRIGGER fail_batch");
    archive.append(batch);
    auto updated = record(2, "group", "补全图片URL");
    updated["segments"].push_back({{"type", "image"}, {"data", {{"url", "https://archive.example/image.png"}}}});
    archive.append(json::array({updated}));
    require(archive.recent(120)["group"].size() == 3 && archive.find("group", "message-2", 4, std::time(nullptr))["text"] == "补全图片URL", "重试或同序号更新失败");
    archive.append(json::array({record(5, "group", "后完成"), record(4, "group", "先开始")}));
    const auto recent = archive.recent(3)["group"];
    require(recent.size() == 3 && recent[0]["sequence"] == 3 && recent[2]["sequence"] == 5, "异步完成次序污染恢复顺序");
    database.run("CREATE TRIGGER prohibit_empty_write BEFORE UPDATE ON metadata BEGIN SELECT RAISE(ABORT,'empty write'); END");
    archive.append(json::array());
    database.run("DROP TRIGGER prohibit_empty_write");
}

void retention(const std::filesystem::path& directory)
{
    ChatArchive archive(directory.string());
    archive.append(json::array({record(1, "group", "有效中文关键词")}));
    Database database(directory);
    eventually([&] { return database.number("SELECT count(*) FROM chunks") > 0; }, "待维护消息未建立检索片段");
    const auto expired = std::to_string(int64_t(std::time(nullptr)) - 31 * 24 * 3600);
    database.run("UPDATE messages SET timestamp=" + expired + ",record=json_set(record,'$.timestamp'," + expired + ");UPDATE chunks SET min_time=" + expired + ",max_time=" + expired);
    require(archive.recent(120).empty() && archive.find("group", "message-1", 2, std::time(nullptr)).is_null(), "清理前过期内容仍可读");
    require(archive.retrieve("group", 2, std::time(nullptr), "中文关键词", {}).empty(), "清理前检索返回过期内容");
    archive.maintain();
    require(database.number("SELECT count(*) FROM messages") == 0 && database.number("SELECT count(*) FROM chunks") == 0 &&
            database.number("SELECT count(*) FROM keywords") == 0 && database.number("SELECT count(*) FROM chunk_messages") == 0,
            "过期原文或关联索引未同步删除");
    require(archive.lastSequence() == 1, "淘汰后最高序号丢失");
}

void retrieval(Embeddings& service, const std::filesystem::path& directory)
{
    const auto now = int64_t(std::time(nullptr));
    ChatArchive archive(directory.string());
    json batch = json::array();
    for(uint64_t group = 1; group <= 5; ++group)
        batch.push_back(record(group, std::to_string(group), "火星探测任务由甲负责", now - 3600));
    batch.push_back(record(6, "1", "厨房烹饪技巧", now - 7200));
    batch.push_back(record(7, "1", "火星当前窗口重复项", now - 1800));
    batch.push_back(record(8, "1", "火星未来消息", now + 100));
    auto image = record(9, "1", "图片旁路 https://archive.example/image.png 中文关键词", now - 10000);
    image["segments"].push_back({{"type", "image"}, {"data", {{"url", "https://archive.example/image.png"}, {"summary", "https://archive.example/image.png"}}}});
    batch.push_back(image);
    archive.append(batch);
    Database database(directory);
    eventually([&] { return database.number("SELECT count(*) FROM messages WHERE indexed=0") == 0 &&
                            database.number("SELECT count(*) FROM chunks WHERE embedding IS NULL") == 0; }, "索引未完成");
    const auto keyword = archive.retrieve("1", 8, now, "火星", {7});
    require(!keyword.empty() && keyword.dump().find("火星探测任务") != std::string::npos, "中文双字关键词未命中");
    const auto semantic = archive.retrieve("1", 8, now, "红色星球的探索安排", {7});
    require(!semantic.empty() && semantic.dump().find("火星探测任务") != std::string::npos, "不同措辞语义检索未命中");
    for(const auto& result : semantic)
        for(const auto& source : result["records"])
            require(source["group_id"] == "1" && source["sequence"].get<uint64_t>() < 8 && source["sequence"] != 7 &&
                    source["timestamp"].get<int64_t>() <= now, "检索混入隔群、未来或重复消息");
    require(archive.retrieve("1", 8, now, "火星", {1, 7}).empty(), "排除窗口后仍注入重复原文");
    const auto protected_image = archive.retrieve("1", 10, now, "中文关键词", {1, 6, 7});
    require(!protected_image.empty() && protected_image.dump().find("https://archive.example/image.png") == std::string::npos,
            "检索片段附带历史原图URL");
    require(!service.leaked_image, "embedding请求泄露历史原图URL");
    const auto calls = service.requests.load();
    require(archive.retrieve("1", 10, now, " \n\t", {}).empty() && service.requests == calls, "无文本仍发embedding查询");
    service.fail = true;
    const auto fallback = archive.retrieve("1", 8, now, "火星", {7});
    require(!fallback.empty(), "embedding失败时中文检索也丢失");
    service.fail = false;
    service.slow = true;
    const auto start = std::chrono::steady_clock::now();
    archive.retrieve("1", 8, now, "红色星球", {7});
    require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1600), "检索网络等待未有界");
    service.slow = false;
    json abundant = json::array();
    for(uint64_t sequence = 10; sequence <= 17; ++sequence)
        abundant.push_back(record(sequence, "1", "火星历史任务" + std::to_string(sequence), now - int64_t(sequence) * 1000));
    archive.append(abundant);
    eventually([&] { return database.number("SELECT count(*) FROM messages WHERE indexed=0") == 0; }, "新增历史未建立关键词片段");
    const auto bounded = archive.retrieve("1", 18, now, "火星", {1, 7});
    require(bounded.size() == 5, "检索未限制为最多五个相关原文片段");
    std::set<uint64_t> unique;
    for(const auto& item : bounded)
        for(const auto& source : item["records"])
            require(unique.insert(source["sequence"].get<uint64_t>()).second, "多个检索片段重复同一原消息");
    archive.shutdown();
}

void recovery(Embeddings& service, const std::filesystem::path& directory)
{
    service.fail = true;
    {
        ChatArchive archive(directory.string());
        archive.append(json::array({record(1, "1", "火星离线归档", std::time(nullptr) - 3600)}));
        Database database(directory);
        eventually([&] { return database.number("SELECT count(*) FROM chunks") == 1; }, "离线时原文未分块");
        require(archive.recent(120)["1"].size() == 1, "embedding失败影响原文归档");
    }
    service.fail = false;
    {
        Database database(directory);
        database.run("UPDATE chunks SET attempted=0");
    }
    {
        ChatArchive archive(directory.string());
        Database database(directory);
        eventually([&] { return database.number("SELECT count(*) FROM chunks WHERE embedding IS NOT NULL") == 1; }, "重启未恢复未完成向量索引");
        require(!archive.retrieve("1", 2, std::time(nullptr), "红色星球", {}).empty(), "恢复索引不可检索");
    }
    EMBEDDING_MODEL = "archive-test-new-model";
    {
        ChatArchive archive(directory.string());
        Database database(directory);
        eventually([&] { return database.number("SELECT count(*) FROM chunks WHERE embedding IS NOT NULL AND model LIKE '%archive-test-new-model'") == 1; }, "模型切换未重建向量索引");
    }
    EMBEDDING_MODEL = "archive-test";
}

void longSources(const std::filesystem::path& directory)
{
    const auto host = EMBEDDING_HOST;
    EMBEDDING_HOST.clear();
    {
        ChatArchive archive(directory.string());
        const std::string text = std::string(2100, 'x') + "关键词" + std::string(2100, 'y');
        archive.append(json::array({record(1, "1", text)}));
        Database database(directory);
        eventually([&] { return database.number("SELECT count(*) FROM chunks") == 3; }, "长消息未按有界原文片段切分");
        const auto found = archive.retrieve("1", 2, std::time(nullptr), "关键词", {});
        require(found.size() == 1 && found[0]["records"][0]["text"].get<std::string>().size() <= 2048,
                "长消息检索回填了完整原文");
        require(archive.find("1", "message-1", 2, std::time(nullptr))["text"] == text, "切分改写了归档原文");
    }
    EMBEDDING_HOST = host;
}

void adjacentRebuild(const std::filesystem::path& directory)
{
    const auto host = EMBEDDING_HOST;
    EMBEDDING_HOST.clear();
    const auto now = int64_t(std::time(nullptr));
    auto updated = record(1, "1", std::string(1500, 'a'), now);
    {
        ChatArchive archive(directory.string());
        archive.append(json::array({updated, record(2, "1", std::string(1500, 'b'), now),
            record(3, "other", "其他群的序号间隔", now), record(4, "1", std::string(1500, 'c'), now),
            record(5, "1", std::string(1500, 'd'), now)}));
        Database database(directory);
        eventually([&] { return database.number("SELECT count(*) FROM chunks") == 3; }, "相邻消息初始片段错误");
        archive.shutdown();
        updated["segments"].push_back({{"type", "image"}, {"data", {{"url", "https://archive.example/image.png"}}}});
        archive.append(json::array({updated, record(6, "1", "新到消息", now)}));
        require(database.number("SELECT count(*) FROM chunks") == 2, "更新未使对应旧片段失效");
    }
    {
        ChatArchive archive(directory.string());
        Database database(directory);
        eventually([&] { return database.number("SELECT count(*) FROM messages WHERE indexed=0") == 0; }, "失效片段未重建");
        require(database.number("SELECT count(*) FROM chunks c WHERE EXISTS(SELECT 1 FROM messages m "
            "WHERE m.group_id=c.group_id AND m.sequence BETWEEN c.start_sequence AND c.end_sequence "
            "AND NOT EXISTS(SELECT 1 FROM chunk_messages cm WHERE cm.chunk_id=c.id AND cm.sequence=m.sequence))") == 0,
            "重建片段跨过同群已索引中间消息");
    }
    EMBEDDING_HOST = host;
}

}

int main()
{
    const auto root = std::filesystem::path("release/tests/archive-data-") += std::to_string(getpid());
    try
    {
        std::filesystem::create_directories(root);
        Embeddings service;
        configure(service, root);
        migration(root / "migration");
        damaged(root);
        transactions(root / "transactions");
        retention(root / "retention");
        retrieval(service, root / "retrieval");
        recovery(service, root / "recovery");
        longSources(root / "long-sources");
        adjacentRebuild(root / "adjacent-rebuild");
        std::filesystem::remove_all(root);
        std::cout << "archive tests passed\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "archive test failed: " << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
