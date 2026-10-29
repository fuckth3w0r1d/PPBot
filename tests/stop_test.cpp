#include "BotServer.h"
#include "ToolManagers/ChatArchive.h"
#include "config.h"
#include "sqlite3.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

static std::filesystem::path fixture_path;

static void loadFixtureConfig()
{
    load_config(fixture_path.string());
}

#define load_config loadFixtureConfig
#define main botMain
#include "../src/main.cpp"
#undef main
#undef load_config

static void require(bool condition, const std::string& message)
{
    if(!condition) throw std::runtime_error(message);
}

static int bindSocket(int& port)
{
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    require(socket >= 0, "测试端口创建失败");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(socket, 1))
    {
        ::close(socket);
        throw std::runtime_error("测试端口绑定失败");
    }
    socklen_t size = sizeof(address);
    if(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size))
    {
        ::close(socket);
        throw std::runtime_error("测试端口读取失败");
    }
    port = ntohs(address.sin_port);
    return socket;
}

static void fixture(const std::filesystem::path& root, int port)
{
    std::filesystem::create_directories(root);
    std::ifstream input("config.example.json");
    auto settings = json::parse(input);
    settings["log"]["file"]["path"] = (root / "bot.log").string();
    settings["log"]["file"]["level"] = 3;
    settings["log"]["console"]["level"] = 3;
    settings["client"]["host"] = "127.0.0.1";
    settings["client"]["port"] = port;
    settings["file"]["data_path"] = (root / "data").string();
    settings["file"]["cache_path"] = (root / "cache").string();
    settings["file"]["save_frequency_seconds"] = 3600;
    settings["embedding"]["host"] = "http://127.0.0.1:1";
    settings["embedding"]["timeout_seconds"] = 1;
    fixture_path = root / "config.json";
    std::ofstream output(fixture_path);
    output << settings;
    require(static_cast<bool>(output), "停机测试配置写入失败");
}

static void waitReady(int port)
{
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(100ms);
    client.set_read_timeout(100ms);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(client.Get("/")) return;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("停机测试HTTP服务未就绪");
}

static int waitChild(pid_t child)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int status = 0;
    while(std::chrono::steady_clock::now() < deadline)
    {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if(result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if(result < 0 && errno != EINTR) throw std::runtime_error("停机测试子进程等待失败");
        std::this_thread::sleep_for(10ms);
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    throw std::runtime_error("停机测试子进程未在期限内退出");
}

static void serverLifecycle(const std::filesystem::path& root)
{
    for(int scenario = 0; scenario < 3; ++scenario)
    {
        int port;
        const int occupied = bindSocket(port);
        fixture(root / std::to_string(scenario), port);
        if(scenario != 2) ::close(occupied);
        const auto child = ::fork();
        require(child >= 0, "停机测试fork失败");
        if(child == 0)
        {
            try
            {
                loadFixtureConfig();
                BotServer bot;
                if(scenario == 2)
                {
                    bool rejected = false;
                    try { bot.start(); } catch(const std::runtime_error&) { rejected = true; }
                    bot.stop();
                    require(rejected, "端口冲突未报告监听失败");
                }
                else
                {
                    auto stopped = std::async(std::launch::async, [&]{
                        if(scenario == 0) waitReady(port);
                        bot.stop();
                    });
                    if(scenario == 1) std::this_thread::sleep_for(20ms);
                    bot.start();
                    stopped.get();
                }
                std::_Exit(bot.tsk_manager.shutdown() ? 0 : 1);
            }
            catch(const std::exception& error)
            {
                std::cerr << error.what() << '\n';
                std::_Exit(1);
            }
        }
        const auto result = waitChild(child);
        if(scenario == 2) ::close(occupied);
        require(result == 0, "正常停止、提前停止或监听失败处理异常");
    }
}

static void signalFlush(const std::filesystem::path& root, int signal, bool fail = false)
{
    int port;
    ::close(bindSocket(port));
    fixture(root, port);
    const auto child = ::fork();
    require(child >= 0, "信号测试fork失败");
    if(child == 0) std::_Exit(botMain());
    bool reaped = false;
    try
    {
        waitReady(port);
        httplib::Client client("127.0.0.1", port);
        client.set_read_timeout(1s);
        const json event = {{"post_type", "message"}, {"message_type", "group"}, {"group_id", 123},
            {"user_id", 77}, {"message_id", 1001}, {"time", std::time(nullptr)},
            {"sender", {{"nickname", "member"}, {"card", "群成员"}}},
            {"message", json::array({{{"type", "record"}, {"data", json::object()}}})}};
        const auto response = client.Post("/", event.dump(), "application/json");
        require(response && response->status == 200, "信号测试普通消息接收失败");
        sqlite3* database = nullptr;
        sqlite3_stmt* statement = nullptr;
        int status = sqlite3_open_v2((root / "data/AiModule/messages.sqlite3").c_str(),
                                    &database, SQLITE_OPEN_READONLY, nullptr);
        if(status == SQLITE_OK)
            status = sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM messages WHERE message_id='1001'",
                                        -1, &statement, nullptr);
        if(status == SQLITE_OK) status = sqlite3_step(statement);
        const auto count = status == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
        sqlite3_finalize(statement);
        if(database) sqlite3_close(database);
        require(status == SQLITE_ROW && count == 0, "普通消息在保存周期之前同步落盘");
        if(fail)
        {
            database = nullptr;
            status = sqlite3_open_v2((root / "data/AiModule/messages.sqlite3").c_str(),
                                    &database, SQLITE_OPEN_READWRITE, nullptr);
            if(status == SQLITE_OK)
                status = sqlite3_exec(database, "CREATE TRIGGER reject_archive BEFORE INSERT ON messages "
                    "BEGIN SELECT RAISE(ABORT,'stop-write-denied'); END", nullptr, nullptr, nullptr);
            if(database) sqlite3_close(database);
            require(status == SQLITE_OK, "最终归档失败场景准备失败");
        }
        require(::kill(child, signal) == 0, "正常停机信号发送失败");
        const int result = waitChild(child);
        reaped = true;
        require(result == (fail ? 1 : 0), "正常信号停机退出状态错误");
        loadFixtureConfig();
        ChatArchive archive((root / "data/AiModule").string());
        const auto groups = archive.recent(120);
        if(fail)
        {
            std::ifstream log(root / "bot.log");
            const std::string logs((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            require(!groups.contains("123") && logs.find("stop-write-denied") != std::string::npos &&
                    logs.find("最终归档失败") != std::string::npos, "最终归档失败未明确报告或留下半批数据");
        }
        else
            require(groups.contains("123") && groups["123"].size() == 1 &&
                    groups["123"][0]["message_id"] == "1001", "正常停机未完成最后一批归档");
    }
    catch(...)
    {
        if(!reaped)
        {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
        }
        throw;
    }
}

int main()
{
    const std::filesystem::path root = "release/tests/stop-runtime";
    bool created = false;
    try
    {
        require(!std::filesystem::exists(root), "停机测试目录已存在");
        std::filesystem::create_directories(root);
        created = true;
        serverLifecycle(root / "server");
        signalFlush(root / "term", SIGTERM);
        signalFlush(root / "int", SIGINT);
        signalFlush(root / "failed", SIGTERM, true);
        std::filesystem::remove_all(root);
        std::cout << "HTTP停止、提前停止、监听失败及SIGTERM/SIGINT最终归档通过\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "停机测试失败: " << error.what() << '\n';
        if(created) std::filesystem::remove_all(root);
        return 1;
    }
}
