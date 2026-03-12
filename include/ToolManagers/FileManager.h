#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <filesystem>
#include <vector>
#include <regex>
#include <atomic>
#include <fstream>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"


using json = nlohmann::json;

class FileManager{
private:
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> file_mutex_map; // 文件锁表
    static std::mutex file_mutex_map_lock; // 保护文件锁表本身
    static std::shared_ptr<std::mutex> getFileMutex(const std::string& path);
    static bool isValidFilename(const std::string& filename);

public:
    static void cleanCache();
    static std::string downloadFile(const std::string& url, const httplib::Headers& headers, const std::string& path, const std::string& filename);
    static bool writeJsonFile(const std::string& path, const json& data);
    static bool readJsonFile(const std::string& path, json& data);
};