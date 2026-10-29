#pragma once
#include <string>

// log
extern std::string LOG_FILE_PATH;
extern std::size_t LOG_FILE_LEVEL;
extern std::size_t LOG_CONSOLE_LEVEL;

// bot
extern std::string BOT_QQ;
extern std::string BOT_NAME;

// server
extern std::string SERVER_HOST;
extern size_t SERVER_PORT;
extern std::string SERVER_TOKEN;

// client
extern std::string CLIENT_HOST;
extern size_t CLIENT_PORT;
extern std::string CLIENT_TOKEN;

extern std::string CHAT_LLM_KEY;
extern std::string CHAT_LLM_HOST;
extern size_t CHAT_LLM_PORT;
extern std::string CHAT_LLM_POST_PATH;
extern std::string CHAT_LLM_MODEL;
extern std::string CHAT_LLM_SYS_PROMPTS;
extern size_t CHAT_LLM_MAX_TOKENS;
extern size_t CHAT_LLM_MAX_MESSAGES;
extern size_t CHAT_LLM_CONTEXT_TOKENS;
extern size_t CHAT_LLM_RETRIEVAL_CONTEXT_TOKENS;
extern size_t CHAT_LLM_TIMEOUT_SECONDS;
extern std::string JUDGE_KEY;
extern std::string JUDGE_HOST;
extern size_t JUDGE_PORT;
extern std::string JUDGE_POST_PATH;
extern std::string JUDGE_MODEL;
extern size_t JUDGE_MAX_TOKENS;
extern size_t JUDGE_TIMEOUT_SECONDS;
extern size_t JUDGE_MAX_MESSAGES;
extern size_t JUDGE_CONTEXT_TOKENS;
extern double CHAT_LLM_REPLY_PROBABILITY;
extern std::string EMBEDDING_KEY;
extern std::string EMBEDDING_HOST;
extern size_t EMBEDDING_PORT;
extern std::string EMBEDDING_POST_PATH;
extern std::string EMBEDDING_MODEL;
extern size_t EMBEDDING_TIMEOUT_SECONDS;

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
