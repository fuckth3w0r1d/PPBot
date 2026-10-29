#include "ToolManagers/ChatArchive.h"
#include "ToolManagers/MessageManager.h"
#include "config.h"
#include "logger.h"
#define SQLITE_CORE
#include "sqlite-vec.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int64_t retention = 30 * 24 * 60 * 60;

int64_t cutoff()
{
    return static_cast<int64_t>(std::time(nullptr)) - retention;
}

void execute(sqlite3* db, const std::string& sql)
{
    char* reason = nullptr;
    if(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &reason) != SQLITE_OK)
    {
        const std::string error = reason ? reason : sqlite3_errmsg(db);
        sqlite3_free(reason);
        throw std::runtime_error("SQLite 执行失败: " + error);
    }
}

class Statement {
    sqlite3* db;
    sqlite3_stmt* value = nullptr;

public:
    Statement(sqlite3* database, const std::string& sql) : db(database)
    {
        if(sqlite3_prepare_v2(db, sql.c_str(), -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error("SQLite 查询准备失败: " + std::string(sqlite3_errmsg(db)));
    }
    ~Statement() { sqlite3_finalize(value); }
    void bind(int index, const std::string& text)
    {
        if(sqlite3_bind_text(value, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("SQLite 文本绑定失败");
    }
    void bind(int index, int64_t number)
    {
        if(sqlite3_bind_int64(value, index, number) != SQLITE_OK)
            throw std::runtime_error("SQLite 数值绑定失败");
    }
    void bind(int index, const std::vector<float>& vector)
    {
        if(sqlite3_bind_blob(value, index, vector.data(), static_cast<int>(vector.size() * sizeof(float)), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("SQLite 向量绑定失败");
    }
    bool next()
    {
        const auto status = sqlite3_step(value);
        if(status == SQLITE_ROW) return true;
        if(status != SQLITE_DONE) throw std::runtime_error("SQLite 查询失败: " + std::string(sqlite3_errmsg(db)));
        return false;
    }
    std::string text(int column) const
    {
        const auto* bytes = sqlite3_column_text(value, column);
        return bytes ? std::string(reinterpret_cast<const char*>(bytes), sqlite3_column_bytes(value, column)) : "";
    }
    int64_t number(int column) const { return sqlite3_column_int64(value, column); }
};

class Transaction {
    sqlite3* db;
    bool committed = false;

public:
    explicit Transaction(sqlite3* database) : db(database) { execute(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if(!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db, "COMMIT"); committed = true; }
};

class QueryLimit {
    sqlite3* db;
    Clock::time_point deadline;

public:
    QueryLimit(sqlite3* database, Clock::time_point end) : db(database), deadline(end)
    {
        sqlite3_progress_handler(db, 500, [](void* value) {
            return Clock::now() >= *static_cast<Clock::time_point*>(value) ? 1 : 0;
        }, &deadline);
        sqlite3_busy_timeout(db, 50);
    }
    ~QueryLimit() { sqlite3_progress_handler(db, 0, nullptr, nullptr); sqlite3_busy_timeout(db, 2000); }
};

std::string utf8Prefix(const std::string& text, size_t length)
{
    if(text.size() <= length) return text;
    while(length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
    return text.substr(0, length);
}

std::vector<std::string> terms(const std::string& text, bool query)
{
    std::set<std::string> values;
    std::string word, previous;
    const auto flush = [&] {
        if(!word.empty()) values.insert(word);
        word.clear();
    };
    for(size_t i = 0; i < text.size();)
    {
        const auto first = static_cast<unsigned char>(text[i]);
        size_t length = first < 128 ? 1 : first < 224 ? 2 : first < 240 ? 3 : 4;
        if(i + length > text.size()) break;
        uint32_t code = first & (length == 1 ? 127 : length == 2 ? 31 : length == 3 ? 15 : 7);
        for(size_t n = 1; n < length; ++n) code = (code << 6) | (static_cast<unsigned char>(text[i+n]) & 63);
        const auto character = text.substr(i, length);
        if((code >= 0x3400 && code <= 0x9fff) || (code >= 0x20000 && code <= 0x323af))
        {
            flush();
            if(!query) values.insert(character);
            if(!previous.empty()) values.insert(previous + character);
            previous = character;
        }
        else
        {
            previous.clear();
            if(first < 128 && std::isalnum(first)) word += static_cast<char>(std::tolower(first));
            else flush();
        }
        i += length;
    }
    flush();
    if(query && values.empty() && !previous.empty()) values.insert(previous);
    return {values.begin(), values.end()};
}

std::string tokenText(const std::string& text, bool query)
{
    std::string result;
    const auto tokens = terms(text, query);
    size_t count = 0;
    for(const auto& token : tokens)
    {
        if(query && count++ == 64) break;
        if(!result.empty()) result += query ? " OR " : " ";
        result += query ? "\"" + token + "\"" : token;
    }
    return result;
}

bool httpUrl(const json& value)
{
    if(!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.compare(0, 7, "http://") == 0 || text.compare(0, 8, "https://") == 0;
}

json normalize(const json& record, const std::string& group = "")
{
    if(!record.is_object()) throw std::runtime_error("群消息记录格式异常");
    for(const auto* field : {"group_id", "message_id", "user_id", "nickname", "card", "text", "reply_to"})
        if(!record.contains(field) || !record[field].is_string()) throw std::runtime_error("群消息字段格式异常");
    if((!group.empty() && record["group_id"] != group) || record["group_id"] == "" ||
       !record.contains("sequence") || !record["sequence"].is_number_unsigned() ||
       record["sequence"].get<uint64_t>() == 0 || record["sequence"].get<uint64_t>() > uint64_t(std::numeric_limits<int64_t>::max()) ||
       !record.contains("timestamp") || !record["timestamp"].is_number_integer() ||
       !record.contains("segments") || !record["segments"].is_array() ||
       !record.contains("bot") || !record["bot"].is_boolean() ||
       !record.contains("reply_to_bot") || !record["reply_to_bot"].is_boolean())
        throw std::runtime_error("群消息边界或顺序异常");
    const auto event = record.value("event_type", std::string("message"));
    if((event != "message" && event != "poke") ||
       (record.contains("target_id") && !record["target_id"].is_string()) ||
       (event == "poke" && (!record.contains("target_id") || record["target_id"] == "" || record["message_id"] != "")))
        throw std::runtime_error("群动作记录格式异常");
    json result = record;
    result["segments"] = json::array();
    for(const auto& segment : record["segments"])
    {
        if(!segment.is_object() || !segment.contains("type") || !segment["type"].is_string()) continue;
        const auto type = segment["type"].get<std::string>();
        const auto data = segment.value("data", json::object());
        json saved = {{"type", type}, {"data", json::object()}};
        if(data.is_object())
        {
            if(type == "image")
            {
                if(data.contains("url") && httpUrl(data["url"])) saved["data"]["url"] = data["url"];
            }
            else
            {
                for(const auto* field : {"text", "summary", "title", "qq", "id"})
                    if(data.contains(field) && (data[field].is_string() || data[field].is_number_integer()))
                        saved["data"][field] = data[field];
                if(type == "json" && data.contains("data") && data["data"].is_string())
                {
                    const auto card = json::parse(data["data"].get<std::string>(), nullptr, false);
                    json available = json::object();
                    if(card.is_object())
                    {
                        if(card.contains("prompt") && card["prompt"].is_string())
                            available["prompt"] = utf8Prefix(card["prompt"].get<std::string>(), 256);
                        if(card.contains("meta") && card["meta"].is_object())
                            for(const auto& [name, detail] : card["meta"].items())
                                if(detail.is_object())
                                    for(const auto* field : {"title", "desc"})
                                        if(detail.contains(field) && detail[field].is_string())
                                            available["meta"][name][field] = utf8Prefix(detail[field].get<std::string>(), 256);
                    }
                    if(!available.empty()) saved["data"]["data"] = available.dump();
                }
            }
        }
        result["segments"].push_back(std::move(saved));
    }
    return result;
}

json hideImages(json record)
{
    std::set<std::string> urls;
    for(const auto& segment : record["segments"])
        if(segment.value("type", "") == "image" && segment.contains("data") && segment["data"].is_object() &&
           segment["data"].contains("url") && httpUrl(segment["data"]["url"]))
            urls.insert(segment["data"]["url"].get<std::string>());
    const auto replace = [&](auto& self, json& value) -> void {
        if(value.is_string())
        {
            auto& text = value.get_ref<std::string&>();
            const std::string omitted = "[图片 URL 已省略]";
            for(const auto& url : urls)
                for(size_t at = 0; (at = text.find(url, at)) != std::string::npos; at += omitted.size())
                    text.replace(at, url.size(), omitted);
        }
        else if(value.is_structured()) for(auto& child : value) self(self, child);
    };
    replace(replace, record);
    for(auto& segment : record["segments"])
        if(segment.value("type", "") == "image") segment["data"] = json::object();
    return record;
}

uint64_t sequenceValue(const std::string& value)
{
    if(value.empty()) return 0;
    if(value.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("归档消息序号元数据异常");
    const auto number = std::stoull(value);
    if(number > uint64_t(std::numeric_limits<int64_t>::max())) throw std::runtime_error("归档消息序号元数据越界");
    return number;
}

std::string endpoint(std::string host)
{
    if(host.find("://") == std::string::npos) host = "https://" + host;
    while(!host.empty() && host.back() == '/') host.pop_back();
    const auto authority = host.substr(host.find("://") + 3);
    const auto bracket = authority.rfind(']');
    const bool port = bracket == std::string::npos ? authority.find(':') != std::string::npos : authority.find(':', bracket) != std::string::npos;
    if(!port) host += ":" + std::to_string(EMBEDDING_PORT);
    return host;
}

}

struct ChatArchive::Impl {
    sqlite3* db = nullptr;
    std::timed_mutex database_mutex;
    std::mutex worker_mutex, client_mutex;
    std::condition_variable condition;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::shared_ptr<httplib::Client> active_client;
    std::string identity;

    explicit Impl(const std::string& directory)
    {
        std::filesystem::create_directories(directory);
        const auto path = directory + "/messages.sqlite3";
        if(std::filesystem::exists(path) && std::filesystem::file_size(path) == 0)
            throw std::runtime_error("SQLite 归档为空字节文件 path=" + path);
        if(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            const std::string reason = db ? sqlite3_errmsg(db) : "无法分配数据库连接";
            if(db) sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("SQLite 归档打开失败 path=" + path + " reason=" + reason);
        }
        try
        {
            sqlite3_busy_timeout(db, 2000);
            {
                Statement check(db, "PRAGMA quick_check");
                if(!check.next() || check.text(0) != "ok") throw std::runtime_error("SQLite 归档完整性检查失败");
            }
            if(sqlite3_vec_init(db, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error("sqlite-vec 初始化失败");
            execute(db, "PRAGMA foreign_keys=ON");
            execute(db, "PRAGMA journal_mode=WAL");
            execute(db, "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                        "CREATE TABLE IF NOT EXISTS messages(sequence INTEGER PRIMARY KEY,group_id TEXT NOT NULL,message_id TEXT NOT NULL,"
                        "timestamp INTEGER NOT NULL,record TEXT NOT NULL,indexed INTEGER NOT NULL DEFAULT 0);"
                        "CREATE INDEX IF NOT EXISTS messages_group ON messages(group_id,sequence);"
                        "CREATE INDEX IF NOT EXISTS messages_id ON messages(group_id,message_id);"
                        "CREATE TABLE IF NOT EXISTS chunks(id INTEGER PRIMARY KEY,group_id TEXT NOT NULL,start_sequence INTEGER NOT NULL,"
                        "end_sequence INTEGER NOT NULL,min_time INTEGER NOT NULL,max_time INTEGER NOT NULL,text TEXT NOT NULL,"
                        "embedding BLOB,model TEXT NOT NULL DEFAULT '',attempted INTEGER NOT NULL DEFAULT 0);"
                        "CREATE INDEX IF NOT EXISTS chunks_group ON chunks(group_id,end_sequence,min_time,max_time);"
                        "CREATE TABLE IF NOT EXISTS chunk_messages(chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,"
                        "sequence INTEGER NOT NULL REFERENCES messages(sequence) ON DELETE CASCADE,text TEXT NOT NULL,"
                        "offset INTEGER NOT NULL,PRIMARY KEY(chunk_id,sequence,offset));"
                        "CREATE INDEX IF NOT EXISTS chunk_messages_sequence ON chunk_messages(sequence);"
                        "CREATE VIRTUAL TABLE IF NOT EXISTS keywords USING fts5(tokens,tokenize='unicode61');");
            migrate(directory + "/group_timeline.json");
            validateStored();
            identity = endpoint(EMBEDDING_HOST) + "\n" + EMBEDDING_POST_PATH + "\n" + EMBEDDING_MODEL;
            if(get("embedding_identity") != identity)
            {
                Transaction transaction(db);
                execute(db, "UPDATE chunks SET embedding=NULL,model='',attempted=0");
                put("embedding_identity", identity);
                put("embedding_dimensions", "");
                transaction.commit();
            }
            maintainUnlocked();
            worker = std::thread([this] { run(); });
        }
        catch(...)
        {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }

    ~Impl()
    {
        stop();
        if(db) sqlite3_close(db);
    }

    std::string get(const std::string& key)
    {
        Statement value(db, "SELECT value FROM metadata WHERE key=?");
        value.bind(1, key);
        return value.next() ? value.text(0) : "";
    }

    void put(const std::string& key, const std::string& value)
    {
        Statement update(db, "INSERT INTO metadata(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        update.bind(1, key);
        update.bind(2, value);
        update.next();
    }

    void eraseChunks(const std::string& predicate)
    {
        execute(db, "UPDATE messages SET indexed=0 WHERE sequence IN (SELECT sequence FROM chunk_messages WHERE chunk_id IN (SELECT id FROM chunks WHERE " + predicate + "))");
        execute(db, "DELETE FROM keywords WHERE rowid IN (SELECT id FROM chunks WHERE " + predicate + ")");
        execute(db, "DELETE FROM chunks WHERE " + predicate);
    }

    void appendUnlocked(const json& records)
    {
        uint64_t maximum = sequenceValue(get("last_sequence"));
        for(const auto& raw : records)
        {
            const auto record = normalize(raw);
            const auto sequence = record["sequence"].get<uint64_t>();
            maximum = std::max(maximum, sequence);
            if(record["timestamp"].get<int64_t>() < cutoff()) continue;
            Statement old(db, "SELECT record FROM messages WHERE sequence=?");
            old.bind(1, static_cast<int64_t>(sequence));
            if(old.next())
            {
                const auto previous = json::parse(old.text(0));
                if(previous["group_id"] != record["group_id"] || previous["message_id"] != record["message_id"])
                    throw std::runtime_error("归档消息序号冲突");
                if(previous == record) continue;
                eraseChunks("id IN (SELECT chunk_id FROM chunk_messages WHERE sequence=" + std::to_string(sequence) + ")");
            }
            Statement insert(db, "INSERT INTO messages(sequence,group_id,message_id,timestamp,record,indexed) VALUES(?,?,?,?,?,0) "
                                 "ON CONFLICT(sequence) DO UPDATE SET record=excluded.record,timestamp=excluded.timestamp,indexed=0");
            insert.bind(1, static_cast<int64_t>(sequence));
            insert.bind(2, record["group_id"].get<std::string>());
            insert.bind(3, record["message_id"].get<std::string>());
            insert.bind(4, record["timestamp"].get<int64_t>());
            insert.bind(5, record.dump());
            insert.next();
        }
        put("last_sequence", std::to_string(maximum));
    }

    void migrate(const std::string& path)
    {
        if(get("timeline_migrated") == "1") return;
        json records = json::array();
        if(std::filesystem::exists(path))
        {
            std::ifstream input(path);
            if(!input) throw std::runtime_error("旧群时间线无法打开 path=" + path);
            const auto data = json::parse(input);
            if(!data.is_null() && !(data.is_object() && data.empty()))
            {
                if(!data.is_object() || !data.contains("group_timeline") || !data["group_timeline"].is_object())
                    throw std::runtime_error("旧群时间线结构异常 path=" + path);
                std::set<uint64_t> sequences;
                for(const auto& [group, history] : data["group_timeline"].items())
                {
                    if(!history.is_array()) throw std::runtime_error("旧群时间线数组异常");
                    uint64_t previous = 0;
                    for(const auto& raw : history)
                    {
                        const auto record = normalize(raw, group);
                        const auto sequence = record["sequence"].get<uint64_t>();
                        if(sequence <= previous || !sequences.insert(sequence).second)
                            throw std::runtime_error("旧群时间线顺序异常");
                        previous = sequence;
                        records.push_back(record);
                    }
                }
            }
        }
        Transaction transaction(db);
        appendUnlocked(records);
        put("timeline_migrated", "1");
        transaction.commit();
    }

    void validateStored()
    {
        Statement foreign_keys(db, "PRAGMA foreign_key_check");
        if(foreign_keys.next()) throw std::runtime_error("SQLite 归档来源关联损坏");
        const auto maximum = sequenceValue(get("last_sequence"));
        Statement records(db, "SELECT sequence,group_id,message_id,timestamp,record FROM messages ORDER BY sequence");
        while(records.next())
        {
            const auto record = normalize(json::parse(records.text(4)), records.text(1));
            if(record["sequence"].get<int64_t>() != records.number(0) || record["message_id"] != records.text(2) ||
               record["timestamp"].get<int64_t>() != records.number(3) || record["sequence"].get<uint64_t>() > maximum)
                throw std::runtime_error("SQLite 归档消息字段损坏");
        }
        execute(db, "INSERT INTO keywords(keywords) VALUES('integrity-check')");
    }

    void maintainUnlocked()
    {
        const auto threshold = std::to_string(cutoff());
        Statement exists(db, "SELECT 1 FROM messages WHERE timestamp<? LIMIT 1");
        exists.bind(1, cutoff());
        if(!exists.next()) return;
        Transaction transaction(db);
        eraseChunks("min_time<" + threshold);
        execute(db, "DELETE FROM messages WHERE timestamp<" + threshold);
        transaction.commit();
    }

    bool makeChunks()
    {
        std::lock_guard<std::timed_mutex> lock(database_mutex);
        Statement pending(db, "SELECT m.record,(SELECT MAX(p.sequence) FROM messages p WHERE p.group_id=m.group_id "
                              "AND p.sequence<m.sequence AND p.timestamp>=?) FROM messages m WHERE m.indexed=0 "
                              "AND m.timestamp>=? ORDER BY m.group_id,m.sequence LIMIT 64");
        pending.bind(1, cutoff());
        pending.bind(2, cutoff());
        std::vector<std::pair<json, int64_t>> records;
        while(pending.next()) records.emplace_back(json::parse(pending.text(0)), pending.number(1));
        if(records.empty()) return false;
        Transaction transaction(db);
        json parts = json::array();
        std::string text, group;
        int64_t minimum = 0, maximum = 0;
        const auto flush = [&] {
            if(parts.empty()) return;
            Statement insert(db, "INSERT INTO chunks(group_id,start_sequence,end_sequence,min_time,max_time,text) VALUES(?,?,?,?,?,?)");
            insert.bind(1, group);
            insert.bind(2, parts.front()["sequence"].get<int64_t>());
            insert.bind(3, parts.back()["sequence"].get<int64_t>());
            insert.bind(4, minimum);
            insert.bind(5, maximum);
            insert.bind(6, text);
            insert.next();
            const auto id = sqlite3_last_insert_rowid(db);
            Statement keywords(db, "INSERT INTO keywords(rowid,tokens) VALUES(?,?)");
            keywords.bind(1, id);
            keywords.bind(2, tokenText(text, false));
            keywords.next();
            for(const auto& part : parts)
            {
                Statement source(db, "INSERT INTO chunk_messages(chunk_id,sequence,text,offset) VALUES(?,?,?,?)");
                source.bind(1, id);
                source.bind(2, part["sequence"].get<int64_t>());
                source.bind(3, part["text"].get<std::string>());
                source.bind(4, part["offset"].get<int64_t>());
                source.next();
            }
            parts = json::array();
            text.clear();
        };
        for(const auto& [record, predecessor] : records)
        {
            const auto current_group = record["group_id"].get<std::string>();
            const auto timestamp = record["timestamp"].get<int64_t>();
            if(group != current_group || (!parts.empty() && (std::abs(timestamp - maximum) > 300 ||
               parts.back()["sequence"].get<int64_t>() != predecessor))) flush();
            group = current_group;
            const auto original = hideImages(record)["text"].get<std::string>();
            for(size_t offset = 0; offset < original.size();)
            {
                const auto piece = utf8Prefix(original.substr(offset), 2048);
                if(!parts.empty() && (parts.size() >= 4 || text.size() + piece.size() + 1 > 4096 ||
                   parts.back()["sequence"] == record["sequence"])) flush();
                if(parts.empty()) minimum = maximum = timestamp;
                minimum = std::min(minimum, timestamp);
                maximum = std::max(maximum, timestamp);
                parts.push_back({{"sequence", record["sequence"]}, {"text", piece}, {"offset", offset}});
                if(!text.empty()) text += '\n';
                text += piece;
                offset += piece.size();
            }
            Statement mark(db, "UPDATE messages SET indexed=1 WHERE sequence=?");
            mark.bind(1, record["sequence"].get<int64_t>());
            mark.next();
        }
        flush();
        transaction.commit();
        return true;
    }

    std::optional<std::vector<float>> embed(const std::string& text, bool background, Clock::time_point deadline)
    {
        if(EMBEDDING_HOST.empty() || EMBEDDING_MODEL.empty() || stopping) return std::nullopt;
        try
        {
            auto client = std::make_shared<httplib::Client>(endpoint(EMBEDDING_HOST));
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if(remaining.count() <= 0) return std::nullopt;
            client->set_connection_timeout(remaining);
            client->set_read_timeout(remaining);
            client->set_write_timeout(remaining);
            client->set_max_timeout(remaining);
            if(background)
            {
                std::lock_guard<std::mutex> lock(client_mutex);
                if(stopping) return std::nullopt;
                active_client = client;
            }
            const auto response = client->Post(EMBEDDING_POST_PATH,
                httplib::Headers{{"Authorization", "Bearer " + EMBEDDING_KEY}},
                json{{"model", EMBEDDING_MODEL}, {"input", text}, {"encoding_format", "float"}}.dump(), "application/json");
            if(background)
            {
                std::lock_guard<std::mutex> lock(client_mutex);
                active_client = nullptr;
            }
            if(stopping) return std::nullopt;
            if(!response)
            {
                Logger::warn("Embedding 网络失败 reason=", httplib::to_string(response.error()));
                return std::nullopt;
            }
            if(response->status != 200)
            {
                Logger::httpError("Embedding", response->status, response->body, "HTTP 响应异常");
                return std::nullopt;
            }
            const auto result = json::parse(response->body);
            if(!result.is_object() || !result.contains("data") || !result["data"].is_array() || result["data"].size() != 1 ||
               !result["data"][0].is_object() || !result["data"][0].contains("embedding") ||
               !result["data"][0]["embedding"].is_array() || result["data"][0]["embedding"].empty() ||
               result["data"][0]["embedding"].size() > 65536)
                throw std::runtime_error("embedding 响应向量结构异常");
            std::vector<float> vector;
            double norm = 0;
            for(const auto& value : result["data"][0]["embedding"])
            {
                if(!value.is_number()) throw std::runtime_error("embedding 响应包含非数值");
                const auto number = value.get<float>();
                if(!std::isfinite(number)) throw std::runtime_error("embedding 响应包含无效数值");
                vector.push_back(number);
                norm += double(number) * number;
            }
            if(norm == 0) throw std::runtime_error("embedding 响应为零向量");
            return vector;
        }
        catch(const std::exception& error)
        {
            if(background)
            {
                std::lock_guard<std::mutex> lock(client_mutex);
                active_client = nullptr;
            }
            if(!stopping) Logger::warn("Embedding 请求失败 reason=", error.what());
            return std::nullopt;
        }
    }

    bool indexOne()
    {
        if(EMBEDDING_HOST.empty() || EMBEDDING_MODEL.empty()) return false;
        int64_t id = 0;
        std::string text;
        {
            std::lock_guard<std::timed_mutex> lock(database_mutex);
            Statement pending(db, "SELECT id,text FROM chunks WHERE embedding IS NULL AND min_time>=? AND attempted<=? ORDER BY attempted,id LIMIT 1");
            pending.bind(1, cutoff());
            pending.bind(2, static_cast<int64_t>(std::time(nullptr)) - 10);
            if(!pending.next()) return false;
            id = pending.number(0);
            text = pending.text(1);
            Statement mark(db, "UPDATE chunks SET attempted=? WHERE id=?");
            mark.bind(1, static_cast<int64_t>(std::time(nullptr)));
            mark.bind(2, id);
            mark.next();
        }
        const auto vector = embed(text, true, Clock::now() + std::chrono::seconds(EMBEDDING_TIMEOUT_SECONDS));
        if(!vector || stopping) return false;
        std::lock_guard<std::timed_mutex> lock(database_mutex);
        const auto dimensions = get("embedding_dimensions");
        if(!dimensions.empty() && dimensions != std::to_string(vector->size()))
        {
            Logger::warn("Embedding 索引失败 reason=", "同一服务模型的向量维度发生变化");
            return false;
        }
        Transaction transaction(db);
        if(dimensions.empty()) put("embedding_dimensions", std::to_string(vector->size()));
        Statement update(db, "UPDATE chunks SET embedding=?,model=? WHERE id=? AND min_time>=?");
        update.bind(1, *vector);
        update.bind(2, identity);
        update.bind(3, id);
        update.bind(4, cutoff());
        update.next();
        transaction.commit();
        return true;
    }

    void run()
    {
        while(!stopping)
        {
            bool progress = false;
            try
            {
                progress = makeChunks();
                if(!stopping && indexOne()) progress = true;
            }
            catch(const std::exception& error)
            {
                if(!stopping) Logger::warn("群历史索引失败 reason=", error.what());
            }
            if(!progress)
            {
                std::unique_lock<std::mutex> lock(worker_mutex);
                condition.wait_for(lock, std::chrono::seconds(1));
            }
        }
    }

    void stop()
    {
        stopping = true;
        {
            std::lock_guard<std::mutex> lock(client_mutex);
            if(active_client) active_client->stop();
        }
        condition.notify_all();
        if(worker.joinable()) worker.join();
    }
};

ChatArchive::ChatArchive(const std::string& directory) : impl(std::make_unique<Impl>(directory)) {}
ChatArchive::~ChatArchive() = default;

json ChatArchive::recent(size_t limit)
{
    std::lock_guard<std::timed_mutex> lock(impl->database_mutex);
    Statement rows(impl->db, "SELECT group_id,record FROM (SELECT group_id,record,sequence,"
                             "row_number() OVER(PARTITION BY group_id ORDER BY sequence DESC) AS position "
                             "FROM messages WHERE timestamp>=? AND timestamp<=?) WHERE position<=? ORDER BY sequence");
    rows.bind(1, cutoff());
    rows.bind(2, static_cast<int64_t>(std::time(nullptr)));
    rows.bind(3, static_cast<int64_t>(std::min<size_t>(limit, std::numeric_limits<int64_t>::max())));
    json groups = json::object();
    while(rows.next())
    {
        const auto group = rows.text(0);
        if(!groups.contains(group)) groups[group] = json::array();
        groups[group].push_back(json::parse(rows.text(1)));
    }
    return groups;
}

uint64_t ChatArchive::lastSequence()
{
    std::lock_guard<std::timed_mutex> lock(impl->database_mutex);
    const auto value = impl->get("last_sequence");
    return sequenceValue(value);
}

json ChatArchive::find(const std::string& group, const std::string& id, uint64_t before, int64_t timestamp)
{
    if(id.empty()) return nullptr;
    std::lock_guard<std::timed_mutex> lock(impl->database_mutex);
    Statement row(impl->db, "SELECT record FROM messages WHERE group_id=? AND message_id=? AND sequence<? AND timestamp>=? AND timestamp<=? ORDER BY sequence DESC LIMIT 1");
    row.bind(1, group);
    row.bind(2, id);
    row.bind(3, static_cast<int64_t>(std::min<uint64_t>(before, std::numeric_limits<int64_t>::max())));
    row.bind(4, cutoff());
    row.bind(5, std::min(timestamp, static_cast<int64_t>(std::time(nullptr))));
    return row.next() ? json::parse(row.text(0)) : json(nullptr);
}

void ChatArchive::append(const json& records)
{
    if(!records.is_array()) throw std::runtime_error("待归档消息必须为数组");
    if(records.empty()) return;
    {
        std::lock_guard<std::timed_mutex> lock(impl->database_mutex);
        Transaction transaction(impl->db);
        impl->appendUnlocked(records);
        transaction.commit();
    }
    impl->condition.notify_all();
}

void ChatArchive::maintain()
{
    std::lock_guard<std::timed_mutex> lock(impl->database_mutex);
    impl->maintainUnlocked();
}

json ChatArchive::retrieve(const std::string& group, uint64_t before, int64_t timestamp,
                           const std::string& query, const std::set<uint64_t>& excluded)
{
    json results = json::array();
    if(query.find_first_not_of(" \t\r\n") == std::string::npos || impl->stopping) return results;
    const auto deadline = Clock::now() + std::chrono::seconds(std::max<size_t>(1, EMBEDDING_TIMEOUT_SECONDS));
    const auto event_time = std::min(timestamp, static_cast<int64_t>(std::time(nullptr)));
    std::map<int64_t, double> scores;
    const auto add = [&](Statement& rows) {
        size_t rank = 0;
        while(rows.next()) scores[rows.number(0)] += 1.0 / (60 + ++rank);
    };
    const auto exclusions = [&] {
        std::string sql;
        if(!excluded.empty())
        {
            sql = " AND c.id NOT IN (SELECT chunk_id FROM chunk_messages WHERE sequence IN (";
            for(auto sequence : excluded) sql += std::to_string(sequence) + ',';
            sql.back() = ')';
            sql += ')';
        }
        return sql;
    }();
    const std::string bounds = " c.group_id=? AND c.end_sequence<? AND c.min_time>=? AND c.max_time<=?" + exclusions;
    const auto bindBounds = [&](Statement& statement, int first) {
        statement.bind(first, group);
        statement.bind(first+1, static_cast<int64_t>(before));
        statement.bind(first+2, cutoff());
        statement.bind(first+3, event_time);
    };
    try
    {
        const auto keywords = tokenText(query, true);
        if(!keywords.empty())
        {
            std::unique_lock<std::timed_mutex> lock(impl->database_mutex, std::defer_lock);
            if(!lock.try_lock_until(deadline)) throw std::runtime_error("检索等待数据库超时");
            QueryLimit limit(impl->db, deadline);
            Statement rows(impl->db, "SELECT c.id FROM keywords JOIN chunks c ON c.id=keywords.rowid WHERE keywords MATCH ? AND" + bounds + " ORDER BY bm25(keywords),c.id DESC LIMIT 30");
            rows.bind(1, keywords);
            bindBounds(rows, 2);
            add(rows);
        }
        const auto vector = impl->embed(utf8Prefix(query, 4096), false, deadline);
        if(vector && Clock::now() < deadline)
        {
            std::unique_lock<std::timed_mutex> lock(impl->database_mutex, std::defer_lock);
            if(!lock.try_lock_until(deadline)) throw std::runtime_error("检索等待数据库超时");
            QueryLimit limit(impl->db, deadline);
            const auto dimensions = impl->get("embedding_dimensions");
            if(!dimensions.empty() && dimensions != std::to_string(vector->size()))
                throw std::runtime_error("查询向量维度与归档索引不一致");
            Statement rows(impl->db, "SELECT c.id FROM chunks c WHERE" + bounds +
                " AND c.model=? AND length(c.embedding)=? AND vec_distance_cosine(c.embedding,?)<=0.35 "
                "ORDER BY vec_distance_cosine(c.embedding,?),c.id DESC LIMIT 30");
            bindBounds(rows, 1);
            rows.bind(5, impl->identity);
            rows.bind(6, static_cast<int64_t>(vector->size() * sizeof(float)));
            rows.bind(7, *vector);
            rows.bind(8, *vector);
            add(rows);
        }
    }
    catch(const std::exception& error)
    {
        Logger::warn("群历史检索失败 reason=", error.what());
    }
    std::vector<std::pair<int64_t, double>> ranked(scores.begin(), scores.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.second != right.second ? left.second > right.second : left.first > right.first;
    });
    std::set<uint64_t> included = excluded;
    try
    {
        std::unique_lock<std::timed_mutex> lock(impl->database_mutex, std::defer_lock);
        const auto materialize_deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(100));
        if(!lock.try_lock_until(materialize_deadline)) return results;
        QueryLimit limit(impl->db, materialize_deadline);
        for(const auto& [id, score] : ranked)
        {
            if(results.size() == 5 || Clock::now() >= materialize_deadline) break;
            Statement rows(impl->db, "SELECT m.record,cm.text,cm.offset FROM chunk_messages cm JOIN messages m ON m.sequence=cm.sequence "
                                     "JOIN chunks c ON c.id=cm.chunk_id WHERE cm.chunk_id=? AND m.group_id=? AND m.sequence<? "
                                     "AND m.timestamp>=? AND m.timestamp<=? AND c.group_id=m.group_id ORDER BY m.sequence,cm.offset");
            rows.bind(1, id);
            rows.bind(2, group);
            rows.bind(3, static_cast<int64_t>(before));
            rows.bind(4, cutoff());
            rows.bind(5, event_time);
            json records = json::array();
            while(rows.next())
            {
                auto record = hideImages(json::parse(rows.text(0)));
                const auto sequence = record["sequence"].get<uint64_t>();
                if(included.count(sequence)) continue;
                record["text"] = rows.text(1);
                record["text_offset"] = rows.number(2);
                for(auto& segment : record["segments"])
                    if(segment["type"] == "image") segment["data"] = json::object();
                    else if(segment["type"] == "text") segment["data"]["text"] = rows.text(1);
                records.push_back(std::move(record));
                included.insert(sequence);
            }
            if(!records.empty()) results.push_back({{"source", "retrieved_history"}, {"records", std::move(records)}});
        }
    }
    catch(const std::exception& error)
    {
        Logger::warn("群历史检索读取失败 reason=", error.what());
    }
    return results;
}

void ChatArchive::shutdown() { impl->stop(); }
