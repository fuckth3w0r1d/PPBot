#pragma once
#include <string>

// log
extern std::string LOG_FILE_PATH;
extern std::size_t LOG_FILE_LEVEL;
extern std::size_t LOG_CONSOLE_LEVEL;

// bot
extern std::string BOT_QQ;

// server
extern std::string SERVER_HOST;
extern size_t SERVER_PORT;
extern std::string SERVER_TOKEN;

// client
extern std::string CLIENT_HOST;
extern size_t CLIENT_PORT;
extern std::string CLIENT_TOKEN;

// ai
extern std::string AI_KEY;
extern std::string AI_KEY2;
extern std::string AI_HOST;
extern std::string AI_HOST2;
extern size_t AI_PORT;
extern std::string AI_POST_PATH;
extern std::string AI_POST_PATH2;
extern std::string AI_MODEL;
extern std::string AI_MODEL2;
extern std::string AI_SYS_PROMPTS;
extern size_t AI_MAX_TOKENS;
extern size_t AI_MAX_CHAT_ROUNDS;

// amap
extern std::string AMAP_KEY;
extern std::string AMAP_HOST;
extern size_t AMAP_PORT;
extern std::string AMAP_GET_PATH;

// b23
extern std::string B23_APPID;
extern std::string B23_HOST;
extern size_t B23_PORT;
extern std::string B23_GET_QUERY_PATH;
extern std::string B23_GET_PLAY_PATH;

// file
extern size_t DOWNLOAD_SIZE_LIMIT;
extern std::string CACHE_PATH;
extern std::string DATA_PATH;
extern size_t SAVE_FREQUENCY;
extern size_t DOWNLOAD_BUFFER_SIZE;
extern size_t CACHE_FILE_LIMIT;

// random_img
extern std::string RANDOM_IMG_HOST;
extern std::string RANDOM_IMG_HOST2;
extern size_t RANDOM_IMG_PORT;
extern size_t RANDOM_IMG_PORT2;
extern std::string RANDOM_IMG_GET_PATH;
extern std::string RANDOM_IMG_GET_PATH2;

// eat
extern std::string EAT_HOST;
extern size_t EAT_PORT;
extern std::string EAT_GET_PATH;

// music

extern std::string MUSIC_HOST;
extern size_t MUSIC_PORT;
extern std::string MUSIC_GET_URL_PATH;
extern std::string MUSIC_GET_ID_PATH;


// 初始化函数
void load_config(const std::string& path = "/home/r3t2/PPBot/config.json");