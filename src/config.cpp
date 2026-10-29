#include "config.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <limits>
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

std::string CHAT_LLM_KEY;
std::string CHAT_LLM_HOST;
size_t CHAT_LLM_PORT;
std::string CHAT_LLM_POST_PATH;
std::string CHAT_LLM_MODEL;
std::string CHAT_LLM_SYS_PROMPTS;
size_t CHAT_LLM_MAX_TOKENS;
size_t CHAT_LLM_MAX_MESSAGES = 120;
size_t CHAT_LLM_CONTEXT_TOKENS = 32000;
size_t CHAT_LLM_RETRIEVAL_CONTEXT_TOKENS = 2048;
size_t CHAT_LLM_TIMEOUT_SECONDS = 60;
std::string JUDGE_KEY;
std::string JUDGE_HOST;
size_t JUDGE_PORT = 443;
std::string JUDGE_POST_PATH;
std::string JUDGE_MODEL;
size_t JUDGE_MAX_TOKENS = 16;
size_t JUDGE_TIMEOUT_SECONDS = 5;
size_t JUDGE_MAX_MESSAGES = 12;
size_t JUDGE_CONTEXT_TOKENS = 4000;
double CHAT_LLM_REPLY_PROBABILITY = 0.1;
std::string EMBEDDING_KEY;
std::string EMBEDDING_HOST;
size_t EMBEDDING_PORT = 443;
std::string EMBEDDING_POST_PATH;
std::string EMBEDDING_MODEL;
size_t EMBEDDING_TIMEOUT_SECONDS = 3;

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
    const auto count = [](const json& object, const char* field, size_t fallback) {
        const auto value = object.value(field, json(fallback));
        if(!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
           value.get<uint64_t>() > std::numeric_limits<size_t>::max())
            throw std::invalid_argument(std::string(field) + " 必须是非负整数");
        return value.get<size_t>();
    };

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

    const auto& chat_llm = data.at("chat_llm");
    CHAT_LLM_KEY = chat_llm.at("key").get<std::string>();
    CHAT_LLM_HOST = chat_llm.at("host").get<std::string>();
    CHAT_LLM_PORT = count(chat_llm, "port", 0);
    CHAT_LLM_POST_PATH = chat_llm.at("path").get<std::string>();
    CHAT_LLM_MODEL = chat_llm.at("model").get<std::string>();
    CHAT_LLM_SYS_PROMPTS = chat_llm.at("system_prompts").get<std::string>();
    CHAT_LLM_MAX_TOKENS = count(chat_llm, "max_tokens", 0);
    CHAT_LLM_MAX_MESSAGES = count(chat_llm, "max_messages", 120);
    CHAT_LLM_CONTEXT_TOKENS = count(chat_llm, "context_tokens", 32000);
    CHAT_LLM_RETRIEVAL_CONTEXT_TOKENS = count(chat_llm, "retrieval_context_tokens", 2048);
    CHAT_LLM_TIMEOUT_SECONDS = count(chat_llm, "timeout_seconds", 60);
    CHAT_LLM_REPLY_PROBABILITY = chat_llm.value("reply_probability", 0.1);
    const auto& judge = data.at("judge");
    JUDGE_KEY = judge.at("key").get<std::string>();
    JUDGE_HOST = judge.at("host").get<std::string>();
    JUDGE_PORT = count(judge, "port", 443);
    JUDGE_POST_PATH = judge.at("path").get<std::string>();
    JUDGE_MODEL = judge.at("model").get<std::string>();
    JUDGE_MAX_TOKENS = count(judge, "max_tokens", 16);
    JUDGE_TIMEOUT_SECONDS = count(judge, "timeout_seconds", 5);
    JUDGE_MAX_MESSAGES = count(judge, "max_messages", 12);
    JUDGE_CONTEXT_TOKENS = count(judge, "context_tokens", 4000);
    const auto& embedding = data.at("embedding");
    EMBEDDING_KEY = embedding.at("key").get<std::string>();
    EMBEDDING_HOST = embedding.at("host").get<std::string>();
    EMBEDDING_PORT = count(embedding, "port", 443);
    EMBEDDING_POST_PATH = embedding.value("path", std::string("/v1/embeddings"));
    EMBEDDING_MODEL = embedding.at("model").get<std::string>();
    EMBEDDING_TIMEOUT_SECONDS = count(embedding, "timeout_seconds", 3);
    if(!std::isfinite(CHAT_LLM_REPLY_PROBABILITY) || CHAT_LLM_REPLY_PROBABILITY < 0 || CHAT_LLM_REPLY_PROBABILITY > 1)
        throw std::invalid_argument("chat_llm.reply_probability 必须在 0 到 1 之间");
    if(CHAT_LLM_MAX_TOKENS == 0 || JUDGE_MAX_TOKENS == 0 || CHAT_LLM_MAX_MESSAGES == 0 ||
       JUDGE_MAX_MESSAGES == 0 || JUDGE_MAX_MESSAGES > CHAT_LLM_MAX_MESSAGES ||
       CHAT_LLM_CONTEXT_TOKENS < 512 || JUDGE_CONTEXT_TOKENS < 512 ||
       CHAT_LLM_MAX_TOKENS > CHAT_LLM_CONTEXT_TOKENS - 512 ||
       JUDGE_MAX_TOKENS > JUDGE_CONTEXT_TOKENS - 512 ||
       JUDGE_CONTEXT_TOKENS > CHAT_LLM_CONTEXT_TOKENS || CHAT_LLM_RETRIEVAL_CONTEXT_TOKENS == 0 ||
       CHAT_LLM_RETRIEVAL_CONTEXT_TOKENS >= CHAT_LLM_CONTEXT_TOKENS - CHAT_LLM_MAX_TOKENS ||
       CHAT_LLM_TIMEOUT_SECONDS == 0 || JUDGE_TIMEOUT_SECONDS == 0 ||
       CHAT_LLM_TIMEOUT_SECONDS > 86400 || JUDGE_TIMEOUT_SECONDS > 86400 ||
       CHAT_LLM_PORT == 0 || CHAT_LLM_PORT > 65535 || JUDGE_PORT == 0 || JUDGE_PORT > 65535)
        throw std::invalid_argument("模型窗口、预算、端口或超时配置无效");
    if(EMBEDDING_HOST.empty() || EMBEDDING_MODEL.empty() || EMBEDDING_POST_PATH.empty() ||
       EMBEDDING_POST_PATH.front() != '/' || EMBEDDING_PORT == 0 || EMBEDDING_PORT > 65535 ||
       EMBEDDING_TIMEOUT_SECONDS == 0 || EMBEDDING_TIMEOUT_SECONDS > 60)
        throw std::invalid_argument("embedding 服务、端口或超时配置无效");

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
    SAVE_FREQUENCY = count(data["file"], "save_frequency_seconds", 0);
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
