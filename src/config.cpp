#include "config.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include "json.hpp"

using json = nlohmann::json;

// 全局配置变量定义
std::string LOG_FILE_PATH;
std::size_t LOG_FILE_LEVEL = 1;      
std::size_t LOG_CONSOLE_LEVEL = 0;  

std::string BOT_QQ;
std::string BOT_NAME = "Bot";

std::string SERVER_HOST;
size_t SERVER_PORT;
std::string SERVER_TOKEN;

std::string CLIENT_HOST;
size_t CLIENT_PORT;
std::string CLIENT_TOKEN;

std::string AI_KEY;
std::string AI_HOST;
size_t AI_PORT;
std::string AI_POST_PATH;
std::string AI_MODEL;
std::string AI_SYS_PROMPTS;
size_t AI_MAX_TOKENS;
size_t AI_MAX_CHAT_ROUNDS;
size_t AI_MAX_MESSAGES = 120;
size_t AI_CONTEXT_BYTES = 64000;
size_t AI_TIMEOUT_SECONDS = 60;
std::string JUDGE_KEY;
std::string JUDGE_HOST;
size_t JUDGE_PORT = 443;
std::string JUDGE_POST_PATH;
std::string JUDGE_MODEL;
size_t JUDGE_MAX_TOKENS = 16;
size_t JUDGE_TIMEOUT_SECONDS = 5;
size_t JUDGE_MAX_MESSAGES = 12;
size_t JUDGE_CONTEXT_BYTES = 12000;
double CHAT_REPLY_PROBABILITY = 0.1;

std::string AMAP_KEY;
std::string AMAP_HOST;
size_t AMAP_PORT;
std::string AMAP_GET_PATH;

std::string B23_APPID;
std::string B23_HOST;
size_t B23_PORT;
std::string B23_GET_QUERY_PATH;
std::string B23_GET_PLAY_PATH;

size_t DOWNLOAD_SIZE_LIMIT;
std::string CACHE_PATH;
std::string DATA_PATH;
size_t SAVE_FREQUENCY;
size_t DOWNLOAD_BUFFER_SIZE;
size_t CACHE_FILE_LIMIT;

std::string RANDOM_IMG_HOST;
std::string RANDOM_IMG_HOST2;
size_t RANDOM_IMG_PORT;
size_t RANDOM_IMG_PORT2;
std::string RANDOM_IMG_GET_PATH;
std::string RANDOM_IMG_GET_PATH2;

std::string EAT_HOST;
size_t EAT_PORT;
std::string EAT_GET_PATH;

std::string MUSIC_HOST;
size_t MUSIC_PORT;
std::string MUSIC_GET_URL_PATH;
std::string MUSIC_GET_ID_PATH;


// 加载函数
void load_config(const std::string& path)
{
    errno = 0;
    std::ifstream file(path);
    if (!file.is_open()) 
    {
        const std::string reason = errno ? std::strerror(errno) : "文件无法读取";
        throw std::runtime_error("配置文件打开失败 path=" + path + " reason=" + reason);
    }
    json data;
    try
    {
        data = json::parse(file);
    }
    catch(const json::parse_error& error)
    {
        throw std::runtime_error("配置 JSON 无效 path=" + path + " byte=" + std::to_string(error.byte));
    }

    // log
    LOG_FILE_PATH = data["log"]["file"]["path"].get<std::string>();
    LOG_FILE_LEVEL = data["log"]["file"]["level"].get<std::size_t>();
    LOG_CONSOLE_LEVEL = data["log"]["console"]["level"].get<std::size_t>();
    // bot
    BOT_QQ = data["bot"]["qq"].get<std::string>();
    BOT_NAME = data["bot"].value("name", "Bot");

    // server
    SERVER_HOST = data["server"]["host"].get<std::string>();
    SERVER_PORT = data["server"]["port"].get<size_t>();
    SERVER_TOKEN = data["server"]["access_token"].get<std::string>();

    // client
    CLIENT_HOST = data["client"]["host"].get<std::string>();
    CLIENT_PORT = data["client"]["port"].get<size_t>();
    CLIENT_TOKEN = data["client"]["access_token"].get<std::string>();

    // ai
    AI_KEY = data["ai"]["key"].get<std::string>();
    AI_HOST = data["ai"]["host"].get<std::string>();
    AI_PORT = data["ai"]["port"].get<size_t>();
    AI_POST_PATH = data["ai"]["path"].get<std::string>();
    AI_MODEL = data["ai"]["model"].get<std::string>();
    AI_SYS_PROMPTS = data["ai"]["system_prompts"].get<std::string>();
    AI_MAX_TOKENS = data["ai"]["max_tokens"].get<size_t>();
    AI_MAX_CHAT_ROUNDS = data["ai"]["max_rounds"].get<size_t>();
    AI_MAX_MESSAGES = data["ai"].value("max_messages", size_t{120});
    AI_CONTEXT_BYTES = data["ai"].value("context_bytes", size_t{64000});
    AI_TIMEOUT_SECONDS = data["ai"].value("timeout_seconds", size_t{60});
    CHAT_REPLY_PROBABILITY = data["ai"].value("reply_probability", 0.1);
    const auto& judge = data.at("judge");
    JUDGE_KEY = judge.at("key").get<std::string>();
    JUDGE_HOST = judge.at("host").get<std::string>();
    JUDGE_PORT = judge.value("port", size_t{443});
    JUDGE_POST_PATH = judge.at("path").get<std::string>();
    JUDGE_MODEL = judge.at("model").get<std::string>();
    JUDGE_MAX_TOKENS = judge.value("max_tokens", size_t{16});
    JUDGE_TIMEOUT_SECONDS = judge.value("timeout_seconds", size_t{5});
    JUDGE_MAX_MESSAGES = judge.value("max_messages", size_t{12});
    JUDGE_CONTEXT_BYTES = judge.value("context_bytes", size_t{12000});
    if(!std::isfinite(CHAT_REPLY_PROBABILITY) || CHAT_REPLY_PROBABILITY < 0 || CHAT_REPLY_PROBABILITY > 1)
        throw std::invalid_argument("ai.reply_probability 必须在 0 到 1 之间");
    if(AI_MAX_TOKENS == 0 || JUDGE_MAX_TOKENS == 0 || AI_MAX_MESSAGES == 0 ||
       JUDGE_MAX_MESSAGES == 0 || JUDGE_MAX_MESSAGES > AI_MAX_MESSAGES ||
       AI_CONTEXT_BYTES < 512 || JUDGE_CONTEXT_BYTES < 512 ||
       AI_MAX_TOKENS > (AI_CONTEXT_BYTES - 512) / 4 ||
       JUDGE_MAX_TOKENS > (JUDGE_CONTEXT_BYTES - 512) / 4 ||
       JUDGE_CONTEXT_BYTES > AI_CONTEXT_BYTES || AI_MAX_CHAT_ROUNDS == 0 ||
       AI_TIMEOUT_SECONDS == 0 || JUDGE_TIMEOUT_SECONDS == 0 ||
       AI_TIMEOUT_SECONDS > 86400 || JUDGE_TIMEOUT_SECONDS > 86400 ||
       AI_PORT == 0 || AI_PORT > 65535 || JUDGE_PORT == 0 || JUDGE_PORT > 65535)
        throw std::invalid_argument("AI 窗口、预算、端口或超时配置无效");

    // amap
    AMAP_KEY = data["amap"]["key"].get<std::string>();
    AMAP_HOST = data["amap"]["host"].get<std::string>();
    AMAP_PORT = data["amap"]["port"].get<int>();
    AMAP_GET_PATH = data["amap"]["path"].get<std::string>();

    // b23
    B23_APPID = data["b23"]["app_id"].get<std::string>();
    B23_HOST = data["b23"]["host"].get<std::string>();
    B23_PORT = data["b23"]["port"].get<int>();
    B23_GET_QUERY_PATH = data["b23"]["query_path"].get<std::string>();
    B23_GET_PLAY_PATH = data["b23"]["play_path"].get<std::string>();

    // file
    DOWNLOAD_SIZE_LIMIT = data["file"]["download_size_limit"].get<size_t>();
    CACHE_PATH = data["file"]["cache_path"].get<std::string>();
    DATA_PATH = data["file"]["data_path"].get<std::string>();
    SAVE_FREQUENCY = data["file"]["save_frequency_seconds"].get<size_t>();
    if(SAVE_FREQUENCY == 0) throw std::invalid_argument("保存间隔必须大于 0");
    DOWNLOAD_BUFFER_SIZE = data["file"]["download_buffer_size"].get<size_t>();
    CACHE_FILE_LIMIT = data["file"]["cache_file_limit"].get<size_t>();

    // random_img
    RANDOM_IMG_HOST = data["random_img"]["host"].get<std::string>();
    RANDOM_IMG_HOST2 = data["random_img2"]["host"].get<std::string>();
    RANDOM_IMG_PORT = data["random_img"]["port"].get<size_t>();
    RANDOM_IMG_PORT2 = data["random_img2"]["port"].get<size_t>();
    RANDOM_IMG_GET_PATH = data["random_img"]["path"].get<std::string>();
    RANDOM_IMG_GET_PATH2 = data["random_img2"]["path"].get<std::string>();

    // eat
    EAT_HOST = data["eat"]["host"].get<std::string>();
    EAT_PORT = data["eat"]["port"].get<size_t>();
    EAT_GET_PATH = data["eat"]["path"].get<std::string>();

    // music
    MUSIC_HOST = data["music"]["host"].get<std::string>();
    MUSIC_PORT = data["music"]["port"].get<size_t>();
    MUSIC_GET_URL_PATH = data["music"]["url_path"].get<std::string>();
    MUSIC_GET_ID_PATH = data["music"]["id_path"].get<std::string>();
}
