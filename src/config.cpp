#include "config.h"
#include <fstream>
#include <iostream>
#include "json.hpp"

using json = nlohmann::json;

// 全局配置变量定义
std::string LOG_FILE_PATH;
std::size_t LOG_FILE_LEVEL = 1;      
std::size_t LOG_CONSOLE_LEVEL = 0;  

std::string BOT_QQ;

std::string SERVER_HOST;
size_t SERVER_PORT;
std::string SERVER_TOKEN;

std::string CLIENT_HOST;
size_t CLIENT_PORT;
std::string CLIENT_TOKEN;

std::string AI_KEY;
std::string AI_KEY2;
std::string AI_HOST;
std::string AI_HOST2;
size_t AI_PORT;
std::string AI_POST_PATH;
std::string AI_POST_PATH2;
std::string AI_MODEL;
std::string AI_MODEL2;
std::string AI_SYS_PROMPTS;
size_t AI_MAX_TOKENS;
size_t AI_MAX_CHAT_ROUNDS;

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
    if(!std::filesystem::exists(path))
    {
        std::cerr << "全局配置文件不存在" << std::endl;
    }
    std::ifstream file(path);
    if (!file.is_open()) 
    {
        std::cerr << "无法打开配置文件" << std::endl;
    }
    json data = json::parse(file);

    // log
    LOG_FILE_PATH = data["log"]["file"]["path"].get<std::string>();
    LOG_FILE_LEVEL = data["log"]["file"]["level"].get<std::size_t>();
    LOG_CONSOLE_LEVEL = data["log"]["console"]["level"].get<std::size_t>();
    // bot
    BOT_QQ = data["bot"]["qq"].get<std::string>();

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
    AI_KEY2 = data["ai2"]["key"].get<std::string>();
    AI_HOST = data["ai"]["host"].get<std::string>();
    AI_HOST2 = data["ai2"]["host"].get<std::string>();
    AI_PORT = data["ai"]["port"].get<size_t>();
    AI_POST_PATH = data["ai"]["path"].get<std::string>();
    AI_POST_PATH2 = data["ai2"]["path"].get<std::string>();
    AI_MODEL = data["ai"]["model"].get<std::string>();
    AI_MODEL2 = data["ai2"]["model"].get<std::string>();
    AI_SYS_PROMPTS = data["ai"]["system_prompts"].get<std::string>();
    AI_MAX_TOKENS = data["ai"]["max_tokens"].get<size_t>();
    AI_MAX_CHAT_ROUNDS = data["ai"]["max_rounds"].get<size_t>();

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