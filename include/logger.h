#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include "json.hpp"
#include "config.h"

using json = nlohmann::json;

class Logger{
private:
    static constexpr const char* COLOR_RESET = "\033[0m";
    static constexpr const char* COLOR_RED = "\033[31m";
    static constexpr const char* COLOR_GREEN = "\033[32m";
    static constexpr const char* COLOR_YELLOW = "\033[33m";
    static constexpr const char* COLOR_BLUE = "\033[34m";

    static std::string current_time()
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local;
        localtime_r(&now, &local);
        std::ostringstream output;
        output << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << ']';
        return output.str();
    }

    static std::string getLevelStr(size_t level)
    {
        if(level == 0) return "[DEBUG]";
        if(level == 1) return "[INFO]";
        if(level == 2) return "[WARN]";
        return "[ERROR]";
    }

    static std::string getColoredLevelStr(size_t level)
    {
        const auto color = level == 0 ? COLOR_BLUE : level == 1 ? COLOR_GREEN : level == 2 ? COLOR_YELLOW : COLOR_RED;
        return std::string(color) + getLevelStr(level) + COLOR_RESET;
    }

    static std::string redact(std::string text)
    {
        std::array<std::string, 5> secrets = {AI_KEY, JUDGE_KEY, SERVER_TOKEN, CLIENT_TOKEN, AMAP_KEY};
        std::sort(secrets.begin(), secrets.end(), [](const auto& first, const auto& second){return first.size() > second.size();});
        for(const auto& secret : secrets)
        {
            if(secret.empty()) continue;
            size_t position = 0;
            while((position = text.find(secret, position)) != std::string::npos)
            {
                text.replace(position, secret.size(), "[REDACTED]");
                position += 10;
            }
        }
        return text;
    }

    static std::string singleLine(const std::string& text)
    {
        std::string result;
        for(unsigned char character : text)
        {
            if(character == '\n') result += "\\n";
            else if(character == '\r') result += "\\r";
            else if(character == '\t') result += "\\t";
            else if(character < 0x20 || character == 0x7f)
            {
                const char* hex = "0123456789abcdef";
                result += "\\x";
                result += hex[character >> 4];
                result += hex[character & 0xf];
            }
            else result += static_cast<char>(character);
        }
        return result;
    }

    static std::string shorten(const std::string& text, size_t limit)
    {
        if(text.size() <= limit) return text;
        const auto suffix = std::string("...");
        size_t end = limit - suffix.size();
        while(end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        return text.substr(0, end) + suffix;
    }

    static std::mutex& getMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    static void print(std::ostream& output, const json& value)
    {
        output << value.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    template<typename T> static void print(std::ostream& output, const T& value)
    {
        output << value;
    }

    template<typename T> static void log(std::ostream& output, size_t level, const std::string& tip, const T& data)
    {
        if(level < LOG_CONSOLE_LEVEL && level < LOG_FILE_LEVEL) return;
        std::ostringstream rendered;
        rendered << tip;
        print(rendered, data);
        auto content = singleLine(redact(rendered.str()));
        if(level >= 2) content = shorten(content, 4096);
        std::lock_guard<std::mutex> lock(getMutex());
        const auto time = current_time();
        if(level >= LOG_CONSOLE_LEVEL) output << time << getColoredLevelStr(level) << ' ' << content << std::endl;
        if(level >= LOG_FILE_LEVEL && !LOG_FILE_PATH.empty())
        {
            errno = 0;
            std::ofstream file(LOG_FILE_PATH, std::ios::app);
            file << time << getLevelStr(level) << ' ' << content << std::endl;
            if(!file)
            {
                const auto code = errno;
                std::cerr << time << "[ERROR] 日志文件写入失败 path=" << singleLine(redact(LOG_FILE_PATH))
                    << " reason=" << (code ? std::strerror(code) : "文件流写入失败") << std::endl;
            }
        }
    }

public:
    template<typename T> static void debug(const std::string& tip, const T& data)
    {
        log(std::cerr, 0, tip, data);
    }

    template<typename T> static void info(const std::string& tip, const T& data)
    {
        log(std::cout, 1, tip, data);
    }

    template<typename T> static void warn(const std::string& tip, const T& data)
    {
        log(std::cerr, 2, tip, data);
    }

    template<typename T> static void error(const std::string& tip, const T& data)
    {
        log(std::cerr, 3, tip, data);
    }

    static void httpError(const std::string& operation, int status, const std::string& body, const std::string& reason = "HTTP 响应异常")
    {
        const auto result = json::parse(body, nullptr, false);
        std::string detail;
        std::set<std::string> fields;
        auto append = [&](const json& object){
            if(!object.is_object()) return;
            for(const auto* field : {"code", "type", "message", "msg", "info", "infocode", "retcode", "request_id", "requestId", "request-id"})
                if(object.contains(field) && object[field].is_primitive() && !object[field].is_null() && fields.insert(field).second)
                {
                    if(!detail.empty()) detail += ' ';
                    detail += std::string(field) + '=' + (object[field].is_string() ? object[field].get<std::string>() : object[field].dump());
                }
        };
        if(result.is_object() && result.contains("error"))
        {
            if(result["error"].is_object()) append(result["error"]);
            else if(result["error"].is_string()) detail = "error=" + result["error"].get<std::string>();
        }
        append(result);
        if(detail.empty()) detail = result.is_string() ? result.get<std::string>() : body;
        detail = shorten(singleLine(redact(detail)), 1536);
        warn(operation + " http=" + std::to_string(status) + " reason=" + reason, detail.empty() ? "" : " detail=" + detail);
    }
};
