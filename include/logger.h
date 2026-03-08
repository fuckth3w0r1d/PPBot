#include <iostream>
#include <string>
#include <mutex>
#include "json.hpp"
#include "config.h"

using json = nlohmann::json;

class Logger{
private:
    // ANSI 颜色代码
    static constexpr const char* COLOR_RESET = "\033[0m";
    static constexpr const char* COLOR_RED = "\033[31m";
    static constexpr const char* COLOR_GREEN = "\033[32m";
    static constexpr const char* COLOR_YELLOW = "\033[33m";
    static constexpr const char* COLOR_BLUE = "\033[34m";

    // 时间戳
    static std::string current_time() 
    {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm buf;
        localtime_r(&in_time_t, &buf); // 线程安全
        std::ostringstream ss;
        ss << std::put_time(&buf, "%Y-%m-%d %H:%M:%S");
        return "[" + ss.str() + "]";
    }
    static std::string getColoredLevelStr(size_t level) 
    {
        if(level == 0) return std::string(COLOR_BLUE) + "[DEBUG]" + std::string(COLOR_RESET);
        if(level == 1) return std::string(COLOR_GREEN) + "[INFO]" + std::string(COLOR_RESET);
        if(level == 2) return std::string(COLOR_YELLOW) + "[WARN]" + std::string(COLOR_RESET);
        if(level == 3) return std::string(COLOR_RED) + "[ERROR]" + std::string(COLOR_RESET);
        return "";
    }
    static std::string getLevelStr(size_t level)
    {
        if(level == 0) return "[DEBUG]";
        if(level == 1) return "[INFO]";
        if(level == 2) return "[WARN]";
        if(level == 3) return "[ERROR]";
        return "";
    }

    template<typename T> static void log(std::ostream& os, const size_t level, const std::string& tip, const T& data) 
    {
        std::lock_guard<std::mutex> lock(getMutex());
        std::string coloredlevelstr = getColoredLevelStr(level);
        std::string levelstr = getLevelStr(level);
        std::string time = current_time();
        if(level >= LOG_CONSOLE_LEVEL)
        {
            // 控制台输出
            os << time << coloredlevelstr << " " << tip;
            print(os, data);
            os << std::endl;
        }
        if(level >= LOG_FILE_LEVEL)
        {
            // 保存到日志文件
            std::ofstream ofs(LOG_FILE_PATH, std::ios::app);
            if (ofs.is_open()) {
                ofs << time << levelstr << " " << tip;
                print(ofs, data);
                ofs << std::endl;
            }
        }
    }

    static void print(std::ostream& os, const nlohmann::json& j) 
    {
        os << "\n" << j.dump(4);
    }

    template<typename T> static void print(std::ostream& os, const T& value) 
    {
        os << value;
    }

    static std::mutex& getMutex() 
    {
        static std::mutex mtx;
        return mtx;
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
};