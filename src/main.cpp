#include <iostream>      
#include <string>        
#include <vector>        
#include <memory>        
#include <unordered_map> 
#include <ctime>         
#include <cstddef>
#include <regex>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <shared_mutex>  
#include <mutex>    
#include <chrono>    
#include <thread> 

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"

#include "config.h"
#include "logger.h"

using json = nlohmann::json;

// 消息结构
struct ParsedMsgSegments{
    bool at_me = false;
    bool has_text = false;
    bool has_image = false;
    bool has_json = false;

    std::string text;        // 文本
    std::string image;       // image url
    json json_data;          // json
};
struct MessageContext{
    std::string group_id;
    std::string user_id;
    std::string msg_type;
    json msg_segments;
    ParsedMsgSegments pmsgsegs;
};

////////////
// 消息管理器
///////////
class MessageManager{
private:
    // 消息段解析
    static ParsedMsgSegments parseMsgSegments(const json& msgsegs)
    {
        ParsedMsgSegments result;
        for(auto& seg : msgsegs)
        {
            if(!seg.contains("type") || !seg.contains("data")) continue;
            std::string type = seg["type"].get<std::string>();
            if(type == "at")
            {
                if(seg["data"].contains("qq") && seg["data"]["qq"] == BOT_QQ) 
                    result.at_me = true;
            }
            if(type == "text")
            {
                if (seg["data"].contains("text"))
                {
                    result.has_text = true;
                    result.text += seg["data"]["text"].get<std::string>();
                }
            }
            if(type == "image")
            {
                result.has_image = true;
                if(seg["data"].contains("url"))
                {
                    result.image = seg["data"]["url"].get<std::string>();
                }
            }
            if(type == "json")
            {
                result.has_json = true;
                if(seg["data"].contains("data"))
                {
                    result.json_data = json::parse(seg["data"]["data"].get<std::string>());
                }
            }
        }
        while(!result.text.empty() && result.text[0] == ' ')
        {   // 去除文本消息的前置空格
            result.text.erase(0, 1);
        }
        return result;
    }
public:
    // 统一发送消息接口
    static void send_msg(const MessageContext& recv, const json& reply)
    {
        httplib::Client cli(SERVER_HOST, SERVER_PORT);
        json normal_segments = json::array();
        json video_segments = json::array();
        // 分类 segment
        for(const auto& seg : reply)
        {
            if(seg.contains("type") && seg["type"] == "video")
                video_segments.push_back(seg);
            else
                normal_segments.push_back(seg);
        }
        httplib::Headers headers = {
            {"Authorization", "Bearer " + SERVER_TOKEN}
        };
        std::string path = (recv.msg_type == "group") ? "/send_group_msg" : "/send_private_msg";
        // 发送普通消息
        if(!normal_segments.empty())
        {
            json body;
            if(recv.msg_type == "group")
                body["group_id"] = recv.group_id;
            else
                body["user_id"] = recv.user_id;

            body["message"] = normal_segments;
            auto res = cli.Post(path, headers, body.dump(), "application/json");
            if(!res)
            {
                Logger::error("消息发送失败", httplib::to_string(res.error()));
            }
            else if(res->status != 200)
            {
                Logger::warn("send_msg HTTP状态码: ", res->status);
                Logger::error("send_msg 异常响应体:", json::parse(res->body).dump(4));
            }
        }
        // 单独发送 video
        for(const auto& seg : video_segments)
        {
            json body;
            if(recv.msg_type == "group")
                body["group_id"] = recv.group_id;
            else
                body["user_id"] = recv.user_id;
            body["message"] = json::array({seg});
            auto res = cli.Post(path, headers, body.dump(), "application/json");
            if(!res)
            {
                Logger::error("视频发送失败", httplib::to_string(res.error()));
            }
            else if(res->status != 200)
            {
                Logger::warn("video HTTP状态码: ", res->status);
                Logger::error("video 异常响应体:", json::parse(res->body).dump(4));
            }
        }
    }
    // 获取消息结构
    static MessageContext getMessageContext(const json& data)
    {
        MessageContext msgctx;
        msgctx.msg_type = data["message_type"].get<std::string>();
        if(msgctx.msg_type == "group") msgctx.group_id = std::to_string(data["group_id"].get<size_t>());
        msgctx.user_id = std::to_string(data["user_id"].get<size_t>());
        msgctx.msg_segments = data["message"].get<json>();
        msgctx.pmsgsegs = parseMsgSegments(msgctx.msg_segments);
        return msgctx;
    }
    // 构造array格式消息
    static json buildMsg(const std::string& msg_type, const std::string& msg_data)
    {
        json result;
        if(msg_type == "at")
        {
            json data;
            data["qq"] = msg_data;
            result["data"] = data;
            result["type"] = msg_type;
        }
        if(msg_type == "text")
        {
            json data;
            data["text"] = msg_data;
            result["data"] = data;
            result["type"] = msg_type;
        }
        if(msg_type == "image" || msg_type == "video")
        {
            json data;
            data["file"] = msg_data;
            result["data"] = data;
            result["type"] = msg_type;
        }
        return result;
    }
};

//////////
// 文件管理器
///////////
class FileManager{
private:
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> file_mutex_map; // 文件锁表
    static std::mutex file_mutex_map_lock; // 保护文件锁表本身
    // 获取文件锁
    static std::shared_ptr<std::mutex> getFileMutex(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(file_mutex_map_lock); // 文件锁表上锁
        if(file_mutex_map.find(path) == file_mutex_map.end())
        {
            file_mutex_map[path] = std::make_shared<std::mutex>();
        }
        return file_mutex_map[path];
    }

    static bool isValidFilename(const std::string& filename)
    {
        // 非空
        if(filename.empty())
        {
            Logger::warn("文件名为空", "");
            return false;
        }
        // 检查文件名结尾空格或点
        if(!filename.empty() && (filename.back() == ' ' || filename.back() == '.'))
        {
            Logger::warn("文件名不能以空格或点结尾: ", filename);
            return false;
        }
        // 检查ASCII控制字符 (0x00-0x1F)
        for(char c : filename)
        {
            if (static_cast<unsigned char>(c) < 32)
            {
                Logger::warn("文件名包含控制字符", filename);
                return false;
            }
        }
        // 非法字符
        std::string illegal_chars = "<>:\"/\\|?*";
        for(char c : illegal_chars)
        {
            if (filename.find(c) != std::string::npos)
            {
                Logger::warn("文件名包含非法字符", filename);
                return false;
            }
        }
        return true;
    }

public:
    // 缓存清理函数
    static void cleanCache()
    {
        std::filesystem::path cache_dir = CACHE_PATH;
        if(!std::filesystem::exists(cache_dir))
        {
            Logger::warn("清理缓存时发现缓存目录不存在: ", cache_dir);
            return;
        }
        // 收集所有文件
        std::vector<std::filesystem::path> files;
        for(const auto& entry : std::filesystem::directory_iterator(cache_dir))
        {
            if(entry.is_regular_file())
            {
                files.push_back(entry.path());
            }
        }
        // 如果文件数量小于等于限制则不清理
        if(files.size() <= CACHE_FILE_LIMIT)
        {
            Logger::info("缓存文件数量可接受, 无需清理, 当前缓存文件数量: ", files.size());
            return;
        }
        // 按最后修改时间排序（最旧的在前）
        std::sort(files.begin(), files.end(), 
                  [](const std::filesystem::path& a, const std::filesystem::path& b){
                      return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
                  });
        // 计算需要删除的数量
        size_t files_to_delete = files.size() - CACHE_FILE_LIMIT;
        Logger::info("缓存文件数量超过限制, 将删除最旧的文件, 需清理文件数: ", files_to_delete);
        // 删除最旧的文件
        for(size_t i = 0; i < files_to_delete; i++) 
        {
            std::error_code ec;
            std::filesystem::remove(files[i], ec);
            if(!ec)
            {
                Logger::info("已删除旧缓存: ", files[i].filename().string()); 
                // 从锁表中移除
                std::lock_guard<std::mutex> lock(file_mutex_map_lock);
                file_mutex_map.erase(files[i].generic_string());
            }
        }
    }
    // 网络文件下载到指定目录
    static std::string downloadFile(const std::string& url, const httplib::Headers& headers, const std::string& path ,const std::string& filename)
    {
        // 检查路径是否存在
        if(!std::filesystem::exists(path))
        {
            Logger::warn("路径不存在", path);
            return "";
        }
        // 检查文件名称
        if(!isValidFilename(filename))
        {
            Logger::warn("文件名不合法: ", filename);
            return "";
        }
        // 使用正则表达式解析url
        std::regex url_pattern(R"(https?://([^/:]+)(:\d+)?(/.*))");
        std::smatch match;
        if(!std::regex_match(url, match, url_pattern))
        {
            Logger::error("URL 解析失败", url);
            return "";
        }
        std::string dhost = match[1];
        std::string dpath = match[3];
        // 获取下载文件路径
        std::filesystem::path save_path = std::filesystem::path(path) / filename;
        std::shared_ptr<std::mutex> file_mutex = getFileMutex(save_path.generic_string());
        std::lock_guard<std::mutex> file_lock(*file_mutex); // 上锁
        if(std::filesystem::exists(save_path))
        {
            Logger::info("目录中已经存在文件", save_path);
            return save_path.generic_string();
        }
        // 建立SSL客户端
        httplib::SSLClient cli(dhost);
        cli.set_follow_location(true);
        cli.enable_server_certificate_verification(false); // 下载关闭证书
        cli.set_read_timeout(60);   // 防止卡死
        cli.set_write_timeout(60);
        // 创建文件
        std::ofstream ofs(save_path, std::ios::binary);
        if (!ofs.is_open())
        {
            Logger::error("文件创建失败: ", save_path.generic_string());
            return "";
        }
        // 设置下载缓冲区
        std::unique_ptr<char[]> buffer(new char[DOWNLOAD_BUFFER_SIZE]);
        ofs.rdbuf()->pubsetbuf(buffer.get(), DOWNLOAD_BUFFER_SIZE);
        // 流式下载
        std::atomic<size_t> downloaded_size{0};
        bool size_limit_exceeded = false;
        auto res = cli.Get(dpath, headers, [&](const char* data, size_t data_length){
                if(downloaded_size.load() + data_length > DOWNLOAD_SIZE_LIMIT)
                {
                    size_limit_exceeded = true;
                    Logger::warn("下载过程中超过大小限制: ", downloaded_size + data_length);
                    return false;  // 返回 false 中断下载
                }
                ofs.write(data, data_length);
                downloaded_size += data_length; 
                return true;
            });
        ofs.close();
        if (!res)
        {
            Logger::error("文件下载失败", httplib::to_string(res.error()));
            std::filesystem::remove(save_path);
            return "";
        }
        if(res->status != 200)
        {
            Logger::warn("文件下载异常 HTTP状态码: ", res->status);
            std::filesystem::remove(save_path);
            return "";
        }
        // 检查是否下载超过大小停止
        if(size_limit_exceeded)
        {
            std::filesystem::remove(save_path);
            return "";
        }
        // 返回路径
        Logger::info("文件已下载到: ", save_path.generic_string());
        return save_path.generic_string(); // generic_string方法, 使用 / 
    }

    // 写入 json 数据文件
    static bool writeJsonFile(const std::string& path, const json& data)
    {
        // 上锁
        std::shared_ptr<std::mutex> file_mutex = getFileMutex(path);
        std::lock_guard<std::mutex> lock(*file_mutex);
        std::ofstream ofs(path);
        if(!ofs.is_open())
        {
            Logger::error("JSON 文件写入时打开失败", path);
            return false;
        }
        ofs << data.dump(4);
        return true;
    }
    // 读取 json 数据文件
    static bool readJsonFile(const std::string& path, json& data)
    {
        if(!std::filesystem::exists(path))
        {
            Logger::warn("JSON 文件不存在", path);
            return false;
        }
        // 上锁
        std::shared_ptr<std::mutex> file_mutex = getFileMutex(path);
        std::lock_guard<std::mutex> lock(*file_mutex);
        std::ifstream ifs(path);
        if(!ifs.is_open())
        {
            Logger::error("JSON 文件读取时打开失败", path);
            return false;
        }
        try
        {
            data = json::parse(ifs);
        }catch(const json::parse_error& e){
            Logger::warn("JSON 文件为空或格式错误", path);
            data = json::object();  // 返回空json
        }
        return true;
    }
};
// 定义文件锁表
std::unordered_map<std::string, std::shared_ptr<std::mutex>> FileManager::file_mutex_map;
std::mutex FileManager::file_mutex_map_lock;

//////////
// 文本指令接口
//////////
class Command{
public:
    virtual std::string name() = 0;
    virtual json execute(const std::string& args) = 0;
    virtual ~Command() = default;
};

///////////
// 各个文本指令
///////////
// 帮助
class HelpCommand : public Command{
private:
    std::string cmd_list;
public:
    HelpCommand(std::string cl)
    {
        cmd_list = cl;
    }
    std::string name() override
    {
        return "帮助";
    }
    json execute(const std::string& args) override
    {
        // 返回维护的指令列表
        return MessageManager::buildMsg("text", "指令格式: @我 指令\n当前支持的指令:\n" + cmd_list);
    }
};
// 时间
class TimeCommand : public Command{
private:
    // 获取当前时间的函数
    std::string getFormattedTime() 
    { 
        time_t now = time(0);
        tm local_time;
        if(localtime_r(&now, &local_time) == nullptr)
        {
            Logger::error("获取本地时间失败", "");
            return "时间获取失败";
        }
        char buffer[80];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_time);
        return std::string(buffer);
    }
public:
    std::string name() override
    {
        return "时间";
    }
    json execute(const std::string& args) override
    {
        return MessageManager::buildMsg("text", getFormattedTime());
    }
};
// 天气
class WeatherCommand : public Command{
private:
    // 简单提取城市名称
    std::string getCityName(const std::string& args)
    {
        if (args.empty()) 
        {
            return "";
        }
        std::string city = args;
        // 去除可能的前置空格
        while(!city.empty() && city[0] == ' ')
        {
            city.erase(0, 1);
        }
        // 去除可能的末尾空格
        while (!city.empty() && city.back() == ' ')
        {
            city.pop_back();
        }
        // 只取第一个城市名称
        size_t pos = city.find(' ');
        if (pos != std::string::npos) 
        {
            city = city.substr(0, pos);
        }
        return city;
    }

    // 天气信息结构体
    struct Weatherinfo{
        std::string city; // 城市
        std::string weather; // 天气
        std::string temper; // 温度
        std::string winddir; // 风向
        std::string windpow; // 风力
        std::string humidity; // 湿度
        std::string reporttime; // 查询时间
    };
    // 获取天气信息
    Weatherinfo getWeatherinfo(const json& raw_data)
    {
        Weatherinfo result;
        const json& data = raw_data["lives"][0];
        result.city = data["province"].get<std::string>() + " "
            + data["city"].get<std::string>();
        result.weather = data["weather"].get<std::string>();
        result.temper = data["temperature"].get<std::string>();
        result.winddir = data["winddirection"].get<std::string>();
        result.windpow = data["windpower"].get<std::string>();
        result.humidity = data["humidity"].get<std::string>();
        result.reporttime = data["reporttime"].get<std::string>();
        return result;
    }
    // 访问Amap api获取信息
    std::string askAmap(const std::string& args)
    {
        std::string city = getCityName(args);
        if(city.empty())
        {
            return "请输入城市名称, 格式: 天气 城市名称";
        }
        httplib::SSLClient cli(AMAP_HOST, AMAP_PORT);
        auto res = cli.Get(AMAP_GET_PATH + "?city=" + city + "&key=" + AMAP_KEY);
        if(!res)
        {
            Logger::error("天气网络请求失败", httplib::to_string(res.error()));
            return "天气网络请求失败";
        }
        if(res->status != 200)
        {
            Logger::warn("天气请求 HTTP状态码: ", res->status);
            Logger::error("天气请求 异常响应体:", json::parse(res->body).dump(4));
            return "天气请求异常";
        }
        // 先解析返回的JSON
        json raw_data = json::parse(res->body);
        if(raw_data["lives"].empty())
        {
            Logger::warn("天气查询返回异常", raw_data.dump(4));
            return "天气查询失败, 请输入正确的格式: 天气 城市名称\n暂时只支持国内城市";
        }
        Weatherinfo winfo = getWeatherinfo(raw_data);
        std::string result = "城市: " + winfo.city;
        result += "\n天气: " + winfo.weather;
        result += "\n温度: " + winfo.temper + "℃";
        result += "\n风向: " + winfo.winddir + "风 " + winfo.windpow + "级";
        result += "\n湿度: " + winfo.humidity + "%";
        result += "\n查询时间: " + winfo.reporttime;
        return result;
    }
public:
    std::string name() override
    {
        return "天气";
    }
    json execute(const std::string& args) override
    {
        return MessageManager::buildMsg("text", askAmap(args));
    }
};

// 随机图片
class RandomImgCommand : public Command{
private:
    std::string getImgUrl()
    {
        httplib::SSLClient cli(RANDOM_IMG_HOST, RANDOM_IMG_PORT);
        auto res = cli.Get(RANDOM_IMG_GET_PATH);
        if(!res)
        {
            Logger::error("随机图片网络请求失败", httplib::to_string(res.error()));
            return "";
        }
        if(res->status != 200)
        {
            Logger::warn("随机图片请求 HTTP状态码: ", res->status);
            Logger::error("随机图片请求 异常响应体:", json::parse(res->body).dump(4));
            return "";
        }
        json data = json::parse(res->body);
        std::string img_url = data["url"].get<std::string>();
        return img_url;
    }

public:
    std::string name() override
    {
        return "随机二次元图片";
    }

    json execute(const std::string& args) override
    {
        return MessageManager::buildMsg("image", getImgUrl());
    }   
};

class MealCommand : public Command{
private:
    std::string getMealWhat()
    {
        httplib::SSLClient cli(EAT_HOST, EAT_PORT);
        auto res = cli.Get(EAT_GET_PATH);
        if(!res)
        {
            Logger::error("今天吃啥api网络请求失败", httplib::to_string(res.error()));
            return "";
        }
        if(res->status != 200)
        {
            Logger::warn("今天吃啥api请求 HTTP状态码: ", res->status);
            Logger::error("今天吃啥api请求 异常响应体:", json::parse(res->body).dump(4));
            return "";
        }
        json data = json::parse(res->body);
        std::string result = data["mealwhat"].get<std::string>();
        return result;
    }

public:
    std::string name() override
    {
        return "吃啥";
    }

    json execute(const std::string& args) override
    {
        return MessageManager::buildMsg("text", getMealWhat());
    }
};



///////////
// 任务管理器接口
///////////
class BaseTaskManager{
public:
    virtual bool canHandle(const MessageContext& msgctx) = 0;
    virtual json handleTask(const MessageContext& msgctx) = 0;
    virtual ~BaseTaskManager() = default;
};

///////////////////// 各个任务管理器
////////////
// 被at的文本命令任务管理器
////////////
class CmdTaskManager : public BaseTaskManager{
private: 
    // 维护一个指令表，用于存储多种可支持的指令
    std::unordered_map<std::string, std::unique_ptr<Command>> cmd_map;
    // 注册一个用指令接口实现的指令
    void registerCommand(std::unique_ptr<Command> cmd)
    { 
        cmd_map[cmd->name()] = std::move(cmd);
    }

public:
    // 用于获取指令列表
    std::string getCommandList()
    {
        std::string cmd_list = "帮助\n"; // 初始 list 带有帮助指令
        for(const auto& cmd : cmd_map)
        {
            cmd_list += cmd.first + "\n"; 
        }
        // 去除最后一个回车符
        cmd_list.pop_back();
        return cmd_list;
    }

    CmdTaskManager()
    {
        // 注册各个指令
        registerCommand(std::make_unique<TimeCommand>());
        registerCommand(std::make_unique<WeatherCommand>());
        registerCommand(std::make_unique<RandomImgCommand>());
        registerCommand(std::make_unique<MealCommand>());
        // 后续文本指令也在此注册
        registerCommand(std::make_unique<HelpCommand>(getCommandList()));
    }

    // 能否处理
    bool canHandle(const MessageContext& msgctx) override
    {
        // 仅能处理群聊中被at的消息
        if(!(msgctx.pmsgsegs.at_me && (msgctx.msg_type == "group"))) return false;
        if(msgctx.pmsgsegs.text.empty()) return false;
        // 先按照空格分割指令名称和参数（解释text时已经去除了前置空格）
        size_t pos = msgctx.pmsgsegs.text.find(' ');
        std::string cmd_name = msgctx.pmsgsegs.text.substr(0, pos);
        return cmd_map.count(cmd_name); // 仅能处理指令表中存在的指令
    }

    // 处理某个被at的文本指令
    json handleTask(const MessageContext& msgctx) override
    {
        // 执行指令
        // 先按照空格分割指令名称和参数
        size_t pos = msgctx.pmsgsegs.text.find(' ');
        std::string cmd_name = msgctx.pmsgsegs.text.substr(0, pos);
        // 去除可能的前置空格
        while(!cmd_name.empty() && cmd_name[0] == ' ')
        {
            cmd_name.erase(0, 1);
        }
        std::string cmd_args = (pos == std::string::npos) ? "" : msgctx.pmsgsegs.text.substr(pos + 1);
        // 去除可能的前置空格
        while(!cmd_args.empty() && cmd_args[0] == ' ')
        {
            cmd_args.erase(0, 1);
        }
        json result = json::array();
        if(cmd_map.count(cmd_name))
        {
            // 调用对应指令
            result.emplace_back(cmd_map[cmd_name]->execute(cmd_args));
            return result;
        }
        result.emplace_back(MessageManager::buildMsg("text", "未知指令: " + cmd_name));
        return result;
    }
};

//////////////
// JSON消息任务管理器
//////////////
class JsonTaskManager : public BaseTaskManager{
private:
    // B站视频信息结构
    struct BVinfo{
        std::string title;
        std::string bvid;
        std::string cid; // 获取视频需要的请求参数
        std::string up; // up主昵称
        std::string face; // 头像url
        std::string url; // 视频URL
        size_t size; // 视频大小
        int view; // 观看次数
        int reply; // 评论数
        int favorite; // 收藏数
        int coin; // 投币数
        int share; // 分享数
        int like; // 点赞数
    };
    // 获取 bvid
    std::string getBVid(const json& data)
    {
        std::string qqdocurl = data["meta"]["detail_1"]["qqdocurl"].get<std::string>();
        // 使用正则表达式解析url
        std::regex url_pattern(R"(https?://([^/:]+)(:\d+)?(/.*))");
        std::smatch url_match;
        if(!std::regex_match(qqdocurl, url_match, url_pattern))
        {
            Logger::error("URL 解析失败", qqdocurl);
            return "";
        }
        // 找到qq分享的b站视频链接的 host 和 path
        std::string host = url_match[1];
        std::string path = url_match[3];
        httplib::SSLClient cli(host, 443); // 默认443端口
        auto res = cli.Head(path); // 这里仅为了获取重定向后的url, 所以用Head
        if(!res)
        {
            Logger::error("B站短链网络请求失败", httplib::to_string(res.error()));
            return "B站视频短链网络请求失败";
        }
        if(res->status != 302)
        {
            Logger::warn("B站短链 HTTP状态码: ", res->status);
            Logger::error("B站短链 异常响应体:", json::parse(res->body).dump(4));
            return "B站视频短链网络请求异常";
        }
        std::string real_url = res->get_header_value("Location"); // 获取重定向后的真正B站url
        // 正则匹配获取bvid
        std::regex bv_pattern(R"(BV[A-Za-z0-9]{10})");
        std::smatch bv_match; 
        if (std::regex_search(real_url, bv_match, bv_pattern))
            return bv_match[0];  // 返回匹配到的BV号
        Logger::error("未找到BV号", real_url);
        return "";
    }
    // 获取B站视频信息
    BVinfo getBVinfo(const json& raw_data)
    {
        BVinfo bvinfo;
        const json& data = raw_data["data"];
        bvinfo.cid = std::to_string(data["cid"].get<size_t>());
        bvinfo.bvid = data["bvid"].get<std::string>();
        bvinfo.title = data["title"].get<std::string>();
        bvinfo.up = data["owner"]["name"].get<std::string>();
        bvinfo.face = data["owner"]["face"].get<std::string>();
        bvinfo.view = data["stat"]["view"].get<int>();
        bvinfo.reply = data["stat"]["reply"].get<int>();
        bvinfo.favorite = data["stat"]["favorite"].get<int>();
        bvinfo.coin = data["stat"]["coin"].get<int>();
        bvinfo.share = data["stat"]["share"].get<int>();
        bvinfo.like = data["stat"]["like"].get<int>();
        getBVUrlandSize(bvinfo.bvid, bvinfo.cid, bvinfo);
        return bvinfo;
    }
    // 获取B站视频直链url和视频大小
    void getBVUrlandSize(const std::string& bvid, const std::string& cid, BVinfo& bvinfo)
    {
        httplib::SSLClient cli(B23_HOST, B23_PORT);
        auto res = cli.Get(B23_GET_PLAY_PATH + "?bvid=" + bvid + "&cid=" + cid);
        if (!res)
        {
            Logger::error("B站播放请求失败", httplib::to_string(res.error()));
            return;
        }
        if(res->status != 200)
        {
            Logger::warn("B站播放api HTTP状态码: ", res->status);
            Logger::error("B站播放api 异常响应体:", json::parse(res->body).dump(4));
            return;
        }
        json data = json::parse(res->body);
        if(data["code"].get<int>() != 0 || data["message"].get<std::string>() != "OK" || data["data"]["durl"].empty())
        {
            Logger::error("获取视频播放信息异常", data);
            return;
        }
        bvinfo.url = data["data"]["durl"][0]["url"].get<std::string>();
        Logger::info("获取视频链接: ", bvinfo.url);
        bvinfo.size = data["data"]["durl"][0]["size"].get<size_t>();
        Logger::info("获取视频大小: ", bvinfo.size);
    }   
    
    // 处理B站视频
    std::pair<std::string, std::string> handleBV(const json& data)
    {
        std::string bvid = getBVid(data);
        Logger::info("Bvid: ", bvid);
        httplib::SSLClient cli(B23_HOST, B23_PORT);
        auto res = cli.Get(B23_GET_QUERY_PATH + "?bvid=" + bvid);
        if (!res)
        {
            Logger::error("B站查询请求失败", httplib::to_string(res.error()));
            return std::make_pair("网络请求失败", "");
        }
        if(res->status != 200)
        {
            Logger::warn("B站查询api HTTP状态码: ", res->status);
            Logger::error("B站查询api 异常响应体:", json::parse(res->body).dump(4));
            return std::make_pair("获取视频信息失败", "");
        }
        json raw_bvinfo = json::parse(res->body);
        if(raw_bvinfo["code"].get<int>() != 0 || raw_bvinfo["message"].get<std::string>() != "OK")
        {
            Logger::error("获取视频信息异常", raw_bvinfo);
            return std::make_pair("获取视频信息异常", "");
        }
        // 解析并处理B站视频信息
        BVinfo bvinfo = getBVinfo(raw_bvinfo);
        std::string result;
        result = "视频标题: " + bvinfo.title;
        result += "\nup主: " + bvinfo.up;
        result += "\nup主头像: " + bvinfo.face;
        result += "\nbvid: " + bvinfo.bvid;
        result += "\n观看次数: " + std::to_string(bvinfo.view);
        result += "\n评论数: " + std::to_string(bvinfo.reply);
        result += "\n收藏数: " + std::to_string(bvinfo.favorite);
        result += "\n投币数: " + std::to_string(bvinfo.coin);
        result += "\n分享数: " + std::to_string(bvinfo.share);
        result += "\n点赞数: " + std::to_string(bvinfo.like);
        // 下载B站视频
        httplib::Headers headers = {
            {"Referer", "https://www.bilibili.com"},
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
        };
        // 先清理缓存
        FileManager::cleanCache();
        std::string video_path = FileManager::downloadFile(bvinfo.url, headers, CACHE_PATH, bvinfo.bvid + ".mp4");
        return std::make_pair(result, video_path);
    }
public:
    bool canHandle(const MessageContext& msgctx) override
    {
        // 当群聊消息类型为 json 时能处理
        return (msgctx.msg_type == "group") && msgctx.pmsgsegs.has_json;
    }
    json handleTask(const MessageContext& msgctx) override
    {
        const json& data = msgctx.pmsgsegs.json_data;
        if(data.contains("meta"))
        {
            if(data["meta"].contains("detail_1"))
            {
                if(data["meta"]["detail_1"]["appid"].get<std::string>() == B23_APPID)
                { // 暂时只处理B站分享视频
                    auto [reply_text, video_path] = handleBV(data);
                    json result = json::array();
                    result.emplace_back(MessageManager::buildMsg("text", reply_text));
                    if(!video_path.empty())
                    {
                        result.emplace_back(MessageManager::buildMsg("video", "file://" + video_path));
                    }else{
                        result.emplace_back(MessageManager::buildMsg("text", "视频下载异常, 可能是视频太大了"));
                    }
                    return result;
                }
            }
        }
        Logger::warn("json 消息内容不符合预期", data);
        return {};
    }
};

// AI对话任务
class ChatTaskManager : public BaseTaskManager{
private:
    // 记录每个 session 对话轮数
    static std::unordered_map<std::string, size_t> session_round_counter;
    static std::shared_mutex session_round_counter_mutex;
    // 记录每个群聊对话轮数
    static std::unordered_map<std::string, size_t> group_round_counter;
    static std::shared_mutex group_round_counter_mutex;
    // 保存 session 对话轮数到数据文件
    static bool saveSessionRoundCounter()
    {
        json data;
        std::shared_lock<std::shared_mutex> rlock(session_round_counter_mutex);
        data["session_round_counter"] = session_round_counter;
        return FileManager::writeJsonFile(DATA_PATH + "/AiModule/session_round_counter.json", data);
    }
    // 加载 session 对话轮数
    bool loadSessionRoundCounter()
    {
        json data;
        if(FileManager::readJsonFile(DATA_PATH + "/AiModule/session_round_counter.json", data))
        {
            if(data.empty())
            {
                Logger::warn("从数据文件中加载会话轮数时文件为空: ", DATA_PATH + "/AiModule/session_round_counter.json");
                return true;
            }
            if(!data.contains("session_round_counter"))
            {
                Logger::warn("从数据文件中加载会话轮数时文件异常: ", DATA_PATH + "/AiModule/session_round_counter.json");
                return false;
            }
            std::lock_guard<std::shared_mutex> lock(session_round_counter_mutex);
            session_round_counter.clear();
            session_round_counter = data["session_round_counter"].get<std::unordered_map<std::string, size_t>>();
            return true;
        }
        return false;
    }

    // 保存群聊对话轮数到数据文件
    static bool saveGroupRoundCounter()
    {
        json data;
        std::shared_lock<std::shared_mutex> rlock(group_round_counter_mutex);
        data["group_round_counter"] = group_round_counter;
        return FileManager::writeJsonFile(DATA_PATH + "/AiModule/group_round_counter.json", data);
    }
    // 加载群聊对话轮数
    bool loadGroupRoundCounter()
    {
        json data;
        if(FileManager::readJsonFile(DATA_PATH + "/AiModule/group_round_counter.json", data))
        {
            if(data.empty())
            {
                Logger::warn("从数据文件中加载会话轮数时文件为空: ", DATA_PATH + "/AiModule/group_round_counter.json");
                return true;
            }
            if(!data.contains("group_round_counter"))
            {
                Logger::warn("从数据文件中加载会话轮数时文件异常: ", DATA_PATH + "/AiModule/group_round_counter.json");
                return false;
            }
            std::lock_guard<std::shared_mutex> lock(group_round_counter_mutex);
            group_round_counter.clear();
            group_round_counter = data["group_round_counter"].get<std::unordered_map<std::string, size_t>>();
            return true;
        }
        return false;
    }

    // AI 会话历史结构体
    struct SessionMemCtx{
        std::string role;
        std::string content;
    };
    // 每个 session 对话历史保存
    static std::unordered_map<std::string, std::vector<SessionMemCtx>> session_memory;
    // 会话历史全局锁
    static std::shared_mutex session_memory_global_lock;
    // 针对每个 session 的锁表
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> session_mutex_map;
    static std::mutex session_mutex_map_lock;
    // 获取对话 session id
    std::string getSessionId(const MessageContext& msgctx)
    {
        if(msgctx.msg_type == "group")
        {
            return "group_" + msgctx.group_id + "_user_" + msgctx.user_id;
        }else{
            return "private_" + msgctx.user_id;
        }
        return "";
    }
    // 获取或者创建 session 对应的锁
    std::shared_ptr<std::mutex> getSessionMutex(const std::string& session_id)
    {
        std::lock_guard<std::mutex> lock(session_mutex_map_lock);
        if(session_mutex_map.find(session_id) == session_mutex_map.end())
        {
            session_mutex_map[session_id] = std::make_shared<std::mutex>();
        }
        return session_mutex_map[session_id];
    }
    // 获取或者创建 session 对应的对话历史
    std::vector<SessionMemCtx> getSessionHistory(const std::string& session_id)
    {
        // 先上一个session_memory的全局写锁
        std::unique_lock<std::shared_mutex> wlock(session_memory_global_lock);
        if(session_memory.count(session_id))
        {   // 如果已经存在则解全局锁
            wlock.unlock();
        }
        // 获取锁
        std::shared_ptr<std::mutex> session_mutex = getSessionMutex(session_id);
        std::lock_guard<std::mutex> session_lock(*session_mutex);
        return session_memory[session_id];
    }
    // 获取某个群聊内所有留存的对话历史
    std::vector<SessionMemCtx> getGroupSessionHistory(const std::string& group_id)
    {
        std::vector<SessionMemCtx> result = {};
        std::vector<std::string> session_ids;
        // 加上全局读锁
        std::shared_lock<std::shared_mutex> rlock(session_memory_global_lock);
        for(auto& session : session_memory)
        {
            const std::string& session_id = session.first;
            session_ids.push_back(session_id);
        }
        rlock.unlock(); // 释放读锁
        for(auto& session_id : session_ids)
        {
            size_t pos = session_id.find(group_id);
            if(pos != std::string::npos)
            {
                // 上锁
                std::shared_ptr<std::mutex> session_mutex = getSessionMutex(session_id);
                std::lock_guard<std::mutex> session_lock(*session_mutex);
                result.insert(result.end(), session_memory[session_id].begin(), session_memory[session_id].end());
            }
        }
        return result;
    }
    // 更新会话历史
    bool UpdateSessionMemory(const std::string& session_id, const std::string& user_input, const std::string& ai_reply)
    {
        SessionMemCtx mem1;
        SessionMemCtx mem2;
        mem1.role = "user";
        mem1.content = user_input;
        mem2.role = "assistant";
        mem2.content = ai_reply;
        // 先上锁
        std::shared_ptr<std::mutex> session_mutex = getSessionMutex(session_id);
        std::lock_guard<std::mutex> session_lock(*session_mutex);
        session_memory[session_id].emplace_back(mem1);
        session_memory[session_id].emplace_back(mem2);
        // 如果历史过多则清理历史并设置返回标志来更新用户画像
        if(session_memory[session_id].size() > AI_MAX_CHAT_ROUNDS * 2) // 每轮两条对话
        {
            session_memory[session_id].erase(session_memory[session_id].begin(), session_memory[session_id].begin()+2);
        }
        return true;
    }
    // 保存 session memory 到数据文件中
    static bool saveSessionMemory()
    {
        json data;
        // 上读锁
        std::shared_lock<std::shared_mutex> rlock(session_memory_global_lock);
        for(auto& [session_id, history] : session_memory)
        {
            for(auto& msg : history)
            {
                data["session_memory"][session_id].push_back({
                    {"role", msg.role},
                    {"content", msg.content}
                });
            }
        }
        return FileManager::writeJsonFile(DATA_PATH + "/AiModule/session_memory.json", data);
    }
    // 从数据文件中加载 session memory
    bool loadSessionMemory()
    {
        json data;
        if(FileManager::readJsonFile(DATA_PATH + "/AiModule/session_memory.json", data))
        {
            if(data.empty())
            {
                Logger::warn("从数据文件中加载 session 时文件为空: ", DATA_PATH + "/AiModule/session_memory.json");
                return true;
            }
            if(!data.contains("session_memory"))
            {
                Logger::warn("从数据文件中加载 session 时文件异常: ", DATA_PATH + "/AiModule/session_memory.json");
                return false;
            }
            std::lock_guard<std::shared_mutex> lock(session_memory_global_lock);
            session_memory.clear();
            for(auto& [session_id, history] : data["session_memory"].items())
            {
                for(auto& mem : history)
                {
                    SessionMemCtx ctx;
                    ctx.role = mem["role"];
                    ctx.content = mem["content"];
                    session_memory[session_id].push_back(ctx);
                }
            }
            return true;
        }
        return false;
    }

    // bot 的人格记忆
    std::string learned_persona;  // 用户影响产生的人格
    // 不同群聊的人格记忆隔离, 用一个表记录
    static std::unordered_map<std::string, std::string> persona_map;
    // 全局锁
    static std::shared_mutex persona_map_global_lock;
    // 锁表
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> persona_mutex_map;
    static std::mutex persona_mutex_map_lock;
    // 获取或者创建群聊人格对应的锁
    std::shared_ptr<std::mutex> getPersonaMutex(const std::string& group_id)
    {
        std::lock_guard<std::mutex> lock(persona_mutex_map_lock);
        if(persona_mutex_map.find(group_id) == persona_mutex_map.end())
        {
            persona_mutex_map[group_id] = std::make_shared<std::mutex>();
        }
        return persona_mutex_map[group_id];
    }
    // 获取或者创建对应群聊的人格记忆
    std::string getBotPersona(const std::string& group_id)
    {
        // 先上一个全局写锁
        std::unique_lock<std::shared_mutex> wlock(persona_map_global_lock);
        if(persona_map.count(group_id))
        {
            wlock.unlock();
        }
        // 上锁
        std::shared_ptr<std::mutex> persona_mutex = getPersonaMutex(group_id);
        std::lock_guard<std::mutex> persona_lock(*persona_mutex);
        return persona_map[group_id];
    }
    // 更新对应群聊的人格记忆
    bool UpdateBotPersona(const std::string& group_id)
    {
        // 让 AI 更新群聊人格记忆
        httplib::SSLClient cli(AI_HOST, AI_PORT);
        json body;
        httplib::Headers headers = {
            {"Authorization", "Bearer " + AI_KEY},
            {"Content-Type", "application/json"}
        };
        json messages = json::array();
        messages.push_back({
            {"role", "system"},
            {"content", "<1>以下是你目前的人格" + getBotPersona(group_id) + "<2>接下来输入一系列某个群聊内用户和bot的对话历史, 请融合并生成新的100字人格, 但不要涉及名字服装等等私人化定制内容"}
        });
        // 获取群聊的留存对话历史
        std::vector<SessionMemCtx> history = getGroupSessionHistory(group_id);
        // 输入对话历史
        for(auto& msg : history)
        {
            messages.push_back({
                {"role", msg.role},
                {"content", msg.content}
            });
        }
        // 构造请求体
        body["model"] = AI_MODEL;
        body["messages"] = messages;
        body["max_tokens"] = AI_MAX_TOKENS;
        auto res = cli.Post(AI_POST_PATH, headers, body.dump(), "application/json");
        if(!res)
        {
            Logger::error("AI网络请求失败", httplib::to_string(res.error()));
            return false;
        }
        if(res->status != 200)
        {
            Logger::warn("AI请求 HTTP状态码: ", res->status);
            Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
            return false;
        }
        json raw_data = json::parse(res->body);
        if (!raw_data.contains("choices") || raw_data["choices"].empty())
        {
            Logger::warn("AI回复异常", raw_data.dump(4));
            return false;
        }
        std::string new_persona = raw_data["choices"][0]["message"]["content"].get<std::string>();
        // 上锁
        std::shared_ptr<std::mutex> persona_mutex = getPersonaMutex(group_id);
        std::lock_guard<std::mutex> persona_lock(*persona_mutex);
        // 更新bot记忆
        persona_map[group_id] = new_persona;
        return true;
    }
    // 保存 bot 人格到数据文件中
    static bool saveBotPersona()
    {
        json data;
        // 上读锁
        std::shared_lock<std::shared_mutex> rlock(persona_map_global_lock);
        data["persona_map"] = persona_map;
        return FileManager::writeJsonFile(DATA_PATH + "/AiModule/persona_map.json", data);
    }
    // 从数据文件中加载 bot 人格
    bool loadBotPersona()
    {
        json data;
        if(FileManager::readJsonFile(DATA_PATH + "/AiModule/persona_map.json", data))
        {
            if(data.empty())
            {
                Logger::warn("从数据文件中加载bot人格时文件为空: ", DATA_PATH + "/AiModule/persona_map.json");
                return true;
            }
            if(!data.contains("persona_map"))
            {
                Logger::warn("从数据文件中加载bot人格时文件异常: ", DATA_PATH + "/AiModule/persona_map.json");
                return false;
            }
            std::lock_guard<std::shared_mutex> lock(persona_map_global_lock);
            persona_map.clear();
            persona_map = data["persona_map"].get<std::unordered_map<std::string, std::string>>();
            return true;
        }
        return false;
    }

    // 给对话过的用户画像
    struct UserProfile{
        std::string nickname; // 昵称
        std::string card; // 群昵称
        std::string description;   // AI总结的人设
    };
    // 不同群聊用户画像的记录
    static std::unordered_map<std::string, std::unordered_map<std::string, UserProfile>> users_profile_map;
    // 一个全局锁
    static std::shared_mutex users_profile_map_global_lock;
    // 群聊用户画像总的锁表
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> users_profile_mutex_map;
    static std::mutex users_profile_mutex_map_lock;
    // 获取或创建群聊用户画像锁
    std::shared_ptr<std::mutex> getUsersProMutex(const std::string& group_id)
    {
        std::lock_guard<std::mutex> lock(users_profile_mutex_map_lock);
        if(users_profile_mutex_map.find(group_id) == users_profile_mutex_map.end())
        {
            users_profile_mutex_map[group_id] = std::make_shared<std::mutex>();
        }
        return users_profile_mutex_map[group_id];
    }
    // 获取或创建群聊用户画像
    std::unordered_map<std::string, UserProfile> getUsersProfile(const std::string& group_id, const std::string& user_id)
    {
        // 先上一个全局写锁
        std::unique_lock<std::shared_mutex> wlock(users_profile_map_global_lock);
        if(users_profile_map.count(group_id))
        {
            wlock.unlock();
        }
        // 上锁
        std::shared_ptr<std::mutex> users_profile_mutex = getUsersProMutex(group_id);
        std::lock_guard<std::mutex> users_profile_lock(*users_profile_mutex);
        if(users_profile_map[group_id].count(user_id) == 0)
        {
            // 第一次见到该用户, 同时更新昵称和群昵称
            httplib::Client cli(SERVER_HOST, SERVER_PORT);
            httplib::Headers headers = {
                {"Authorization", "Bearer " + SERVER_TOKEN}
            };
            json body;
            body["group_id"] = group_id;
            body["user_id"] = user_id;
            auto res = cli.Post("/get_group_member_info", headers, body.dump(), "application/json");
            if(!res)
            {
                Logger::error("获取群聊成员信息请求失败", httplib::to_string(res.error()));
                return {};
            }
            if(res->status != 200)
            {
                Logger::warn("获取群聊成员信息请求 HTTP状态码: ", res->status);
                Logger::error("获取群聊成员信息请求 异常响应体:", json::parse(res->body).dump(4));
                return {};
            }
            json raw_data = json::parse(res->body);
            // 更新画像
            users_profile_map[group_id][user_id].card = raw_data["data"]["card"];
            users_profile_map[group_id][user_id].nickname = raw_data["data"]["nickname"];
        }
        return users_profile_map[group_id];
    }
    // 更新群聊用户画像
    bool UpdateUserProfile(const std::string& session_id, const std::string& user_id, const std::string& group_id)
    {
        // 让 AI 总结用户画像
        httplib::SSLClient cli(AI_HOST, AI_PORT);
        json body;
        httplib::Headers headers = {
            {"Authorization", "Bearer " + AI_KEY},
            {"Content-Type", "application/json"}
        };
        json messages = json::array();
        messages.push_back({
            {"role", "system"},
            {"content", "接下来输入一系列user和assistant的对话历史, 请你返回一句话概括user特点"}
        });
        // 获取对话历史
        std::vector<SessionMemCtx> history = getSessionHistory(session_id);
        // 输入对话历史
        for(auto& msg : history)
        {
            messages.push_back({
                {"role", msg.role},
                {"content", msg.content}
            });
        }
        // 构造请求体
        body["model"] = AI_MODEL;
        body["messages"] = messages;
        body["max_tokens"] = AI_MAX_TOKENS;
        auto res = cli.Post(AI_POST_PATH, headers, body.dump(), "application/json");
        if(!res)
        {
            Logger::error("AI网络请求失败", httplib::to_string(res.error()));
            return false;
        }
        if(res->status != 200)
        {
            Logger::warn("AI请求 HTTP状态码: ", res->status);
            Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
            return false;
        }
        json raw_data = json::parse(res->body);
        if (!raw_data.contains("choices") || raw_data["choices"].empty())
        {
            Logger::warn("AI回复异常", raw_data.dump(4));
            return false;
        }
        std::string des = raw_data["choices"][0]["message"]["content"].get<std::string>();
        // 同时更新昵称和群昵称
        httplib::Client cli2(SERVER_HOST, SERVER_PORT);
        httplib::Headers headers2 = {
            {"Authorization", "Bearer " + SERVER_TOKEN}
        };
        json body2;
        body2["group_id"] = group_id;
        body2["user_id"] = user_id;
        auto res2 = cli2.Post("/get_group_member_info", headers2, body2.dump(), "application/json");
        if(!res2)
        {
            Logger::error("获取群聊成员信息请求失败", httplib::to_string(res2.error()));
            return false;
        }
        if(res2->status != 200)
        {
            Logger::warn("获取群聊成员信息请求 HTTP状态码: ", res2->status);
            Logger::error("获取群聊成员信息请求 异常响应体:", json::parse(res2->body).dump(4));
            return false;
        }
        json raw_data2 = json::parse(res2->body);
        // 上锁
        std::shared_ptr<std::mutex> users_profile_mutex = getUsersProMutex(group_id);
        std::lock_guard<std::mutex> users_profile_lock(*users_profile_mutex);
        // 更新画像
        users_profile_map[group_id][user_id].description = des;
        users_profile_map[group_id][user_id].card = raw_data2["data"]["card"];
        users_profile_map[group_id][user_id].nickname = raw_data2["data"]["nickname"];
        return true;
    }
    // 保存用户画像到数据文件中
    static bool saveUsersProfile()
    {
        json data;
        // 上读锁
        std::shared_lock<std::shared_mutex> rlock(users_profile_map_global_lock);
        for(auto& [group_id, users] : users_profile_map)
        {
            for(auto& [user_id, user] : users)
            {
                data["users_profile_map"][group_id][user_id] = {
                    {"nickname", user.nickname},
                    {"card", user.card},
                    {"description", user.description}
                };
            }
        }
        return FileManager::writeJsonFile(DATA_PATH + "/AiModule/users_profile_map.json", data);
    }
    // 从数据文件中加载用户画像
    bool loadUserProfile()
    {
        json data;
        if(FileManager::readJsonFile(DATA_PATH + "/AiModule/users_profile_map.json", data))
        {
            if(data.empty())
            {
                Logger::warn("从数据文件中加载用户画像时文件为空: ", DATA_PATH + "/AiModule/users_profile_map.json");
                return true;
            }
            if(!data.contains("users_profile_map"))
            {
                Logger::warn("从数据文件中加载用户画像时文件异常: ", DATA_PATH + "/AiModule/users_profile_map.json");
                return false;
            }
            std::lock_guard<std::shared_mutex> lock(users_profile_map_global_lock);
            users_profile_map.clear();
            for(auto& [group_id, users] : data["users_profile_map"].items())
            {
                for(auto& [user_id, user] : users.items())
                {
                    UserProfile p;
                    p.nickname = user["nickname"];
                    p.card = user["card"];
                    p.description = user["description"];
                    users_profile_map[group_id][user_id] = p;
                }
            }
            return true;
        }
        return false;
    }


    //与chatAI交互
    std::string ChatWithAI(const MessageContext& msgctx)
    {
        const std::string& user_input = msgctx.pmsgsegs.text;
        const std::string session_id = getSessionId(msgctx);
        httplib::SSLClient cli(AI_HOST, AI_PORT);
        json body;
        httplib::Headers headers = {
            {"Authorization", "Bearer " + AI_KEY},
            {"Content-Type", "application/json"}
        };
        // 获取或者创建对话历史
        std::vector<SessionMemCtx> history = getSessionHistory(session_id);
        Logger::info("获取对应 session 对话历史成功, 轮数: ", history.size()/2);
        // 获取或者创建bot人格
        std::string persona = getBotPersona(msgctx.group_id);
        Logger::info("获取bot人格成功", "");
        // 获取或创建群聊用户画像
        std::unordered_map<std::string, UserProfile> users = getUsersProfile(msgctx.group_id, msgctx.user_id);
        Logger::info("获取群聊用户画像成功, 画像人数: ", users.size());
        json messages = json::array();
        // 加入系统提示词
        messages.push_back({
            {"role", "system"},
            {"content", "<1>" + AI_SYS_PROMPTS}
        });
        // 加入bot人格
        messages.push_back({
            {"role", "system"},
            {"content", "<2>接下来输入你的<群聊习得人格>"}
        });
        messages.push_back({
            {"role","system"},
            {"content", persona}
        });
        // 加入群聊用户画像
        messages.push_back({
            {"role", "system"},
            {"content", "<3>接下来输入一系列用户画像, 格式为 [id:xxx]_[nickname:xxx]_[des:xxx] , 三个[]内xxx分别对应用户id、昵称和描述"}
        });
        for(auto& user : users)
        {
            messages.push_back({
                {"role", "system"},
                {"content", "[id:" + user.first + "]_[nickname:" + user.second.nickname + "]_[des:" + user.second.description + "]"}
            });
        }
        // 加入对话历史
        messages.push_back({
            {"role", "system"},
            {"content", "<4>接下来输入你和用户id: " + msgctx.user_id + "的对话历史"}
        });
        for(auto& msg : history)
        {
            messages.push_back({
                {"role", msg.role},
                {"content", msg.content}
            });
        }
        // 加入用户输入
        messages.push_back({
            {"role", "system"},
            {"content", "<5>接下来输入用户id: " + msgctx.user_id + "的新提问内容"} 
        });
        messages.push_back({
            {"role", "user"},
            {"content", user_input}
        });
        // 构造请求体
        body["model"] = AI_MODEL;
        body["messages"] = messages;
        body["max_tokens"] = AI_MAX_TOKENS;
        auto res = cli.Post(AI_POST_PATH, headers, body.dump(), "application/json");
        // 硅基流动 post 请求示例
        //         curl --request POST \
        //   --url https://api.siliconflow.cn/v1/chat/completions \
        //   -H "Content-Type: application/json" \
        //   -H "Authorization: Bearer YOUR_API_KEY" \
        //   -d '{
        //     "model": "Pro/zai-org/GLM-4.7",
        //     "messages": [
        //       {"role": "system", "content": "你是一个有用的助手"},
        //       {"role": "user", "content": "你好，请介绍一下你自己"}
        //     ]
        //   }'
        if(!res)
        {
            Logger::error("AI网络请求失败", httplib::to_string(res.error()));
            return "AI网络请求失败";
        }
        if(res->status != 200)
        {
            Logger::warn("AI请求 HTTP状态码: ", res->status);
            Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
            return "AI请求异常";
        }
        json raw_data = json::parse(res->body);
        if (!raw_data.contains("choices") || raw_data["choices"].empty())
        {
            Logger::warn("AI回复异常", raw_data.dump(4));
            return "AI 回复异常";
        }
        std::string ai_reply = raw_data["choices"][0]["message"]["content"].get<std::string>();
        Logger::info("获取ai回复成功, 长度: ", ai_reply.length()); 
        if(UpdateSessionMemory(session_id, user_input, ai_reply))
        {
            // 上锁，更新轮数
            std::lock_guard<std::shared_mutex> lock(session_round_counter_mutex);
            session_round_counter[session_id]++;
            // 同步轮数
            Logger::info("更新对应 session 对话历史成功, 轮数: ", session_round_counter[session_id]);
            if(session_round_counter[session_id] >= AI_MAX_CHAT_ROUNDS/10)
            {
                if(!UpdateUserProfile(session_id, msgctx.user_id, msgctx.group_id))
                {
                    Logger::warn("更新用户画像失败, session_id: ", session_id);
                }else{
                    // 重置记录的轮数
                    session_round_counter[session_id] = 0;
                    Logger::info("用户画像已更新, 对应 session 轮数已重置, session_id: ", session_id);
                }
            }
        }
        std::lock_guard<std::shared_mutex> group_round_lock(group_round_counter_mutex);
        group_round_counter[msgctx.group_id]++;
        Logger::info("当前群聊轮数: ", group_round_counter[msgctx.group_id]);
        if(group_round_counter[msgctx.group_id] >= AI_MAX_CHAT_ROUNDS)
        {
            if(!UpdateBotPersona(msgctx.group_id))
            {
                Logger::warn("更新bot群聊人格失败, group_id: ", msgctx.group_id);
            }else{
                group_round_counter[msgctx.group_id] = 0;
                Logger::info("更新bot群聊人格成功, 群聊轮数已重置, group_id: ", msgctx.group_id);
            }
        }
        return ai_reply;
    }
    // 单纯的与纯净模型交互 (豆包)
    std::string askAI(const MessageContext& msgctx)
    {
        httplib::SSLClient cli(AI_HOST2, AI_PORT);
        cli.enable_server_certificate_verification(true);
        json body;
        httplib::Headers headers = {
            {"Authorization", "Bearer " + AI_KEY2},
            {"Expect", ""}
        };
        json messages = json::array();
        // messages.push_back({
        //     {"role", "system"},
        //     {"content", "You are a helpful assistant."}
        // });
        json input = json::array();
        input.push_back({
            {"type", "text"},
            {"text", msgctx.pmsgsegs.text}
        });
        if(msgctx.pmsgsegs.has_image)
        {
            input.push_back({
                {"type", "image_url"},
                {"image_url", {
                    {"url", msgctx.pmsgsegs.image}
                }}
            });
        }
        messages.push_back({
            {"role", "user"},
            {"content", input}
        });
        // 构造请求体
        body["model"] = AI_MODEL2;
        body["messages"] = messages;
        auto res = cli.Post(AI_POST_PATH2, headers, body.dump(), "application/json");
        if(!res)
        {
            Logger::error("AI网络请求失败", httplib::to_string(res.error()));
            return "AI网络请求失败";
        }
        if(res->status != 200)
        {
            Logger::warn("AI请求 HTTP状态码: ", res->status);
            Logger::error("AI请求 异常响应体:", json::parse(res->body).dump(4));
            return "AI请求异常";
        }
        json raw_data = json::parse(res->body);
        if (!raw_data.contains("choices") || raw_data["choices"].empty())
        {
            Logger::warn("AI回复异常", raw_data.dump(4));
            return "AI 回复异常";
        }
        std::string ai_reply = raw_data["choices"][0]["message"]["content"].get<std::string>();
        Logger::info("获取ai回复成功, 长度: ", ai_reply.length());
        return ai_reply;
    }

    // 定时自动保存数据
    static bool autoSaveData()
    {
        if(!dirty_bit.load()) return true; // 数据无变化不需要写入文件
        return saveSessionRoundCounter() && saveSessionMemory() &&
                saveGroupRoundCounter() && saveBotPersona() && saveUsersProfile();
    }

    // 数据脏位
    static std::atomic<bool> dirty_bit;
    // 计时
    static std::thread timer;
    static std::atomic<bool> running;

public:
    ChatTaskManager()
    {
        loadSessionRoundCounter();
        loadGroupRoundCounter();
        Logger::info("对话数量已加载", "");
        loadSessionMemory();
        Logger::info("会话历史已加载", "");
        loadBotPersona();
        Logger::info("Bot 人格已加载", "");
        loadUserProfile();
        Logger::info("用户画像已加载", "");
        dirty_bit.store(false);
        Logger::info("脏位已载入", "");
        running.store(true);
        timer = std::thread([this]{
            while(running)
            {
                autoSaveData();
                std::this_thread::sleep_for(std::chrono::seconds(SAVE_FREQUENCY));
            }
        });
        Logger::info("计时保存线程已启动", "");
    }
    bool canHandle(const MessageContext& msgctx) override
    {
        return msgctx.pmsgsegs.at_me && (msgctx.msg_type == "group");
    }    

    json handleTask(const MessageContext& msgctx) override
    {
        json result = json::array();
        result.emplace_back(MessageManager::buildMsg("at", msgctx.user_id));
        if(msgctx.pmsgsegs.text.empty())
        {
            result.emplace_back(MessageManager::buildMsg("text", "请输入对话内容, 以/起始的对话内容由无上下文AI直接处理, 输入 帮助 可查看命令列表"));
            return result;
        }
        if(msgctx.pmsgsegs.text[0] == '/')
        {
            result.emplace_back(MessageManager::buildMsg("text", askAI(msgctx)));
        }else{
            result.emplace_back(MessageManager::buildMsg("text", ChatWithAI(msgctx)));
        }
        dirty_bit.store(true, std::memory_order_relaxed);
        return result;
    }

    ~ChatTaskManager()
    {
        // 停止计时线程
        running.store(false);
        if(timer.joinable()) timer.join();
    }
};
std::unordered_map<std::string, std::vector<ChatTaskManager::SessionMemCtx>> ChatTaskManager::session_memory;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> ChatTaskManager::session_mutex_map;
std::mutex ChatTaskManager::session_mutex_map_lock;
std::shared_mutex ChatTaskManager::session_memory_global_lock;

std::unordered_map<std::string, std::string> ChatTaskManager::persona_map;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> ChatTaskManager::persona_mutex_map;
std::mutex ChatTaskManager::persona_mutex_map_lock;
std::shared_mutex ChatTaskManager::persona_map_global_lock;

std::unordered_map<std::string, std::unordered_map<std::string, ChatTaskManager::UserProfile>> ChatTaskManager::users_profile_map;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> ChatTaskManager::users_profile_mutex_map;
std::mutex ChatTaskManager::users_profile_mutex_map_lock;
std::shared_mutex ChatTaskManager::users_profile_map_global_lock;

std::unordered_map<std::string, size_t> ChatTaskManager::session_round_counter;
std::shared_mutex ChatTaskManager::session_round_counter_mutex;
std::unordered_map<std::string, size_t> ChatTaskManager::group_round_counter;
std::shared_mutex ChatTaskManager::group_round_counter_mutex;

std::atomic<bool> ChatTaskManager::dirty_bit;
std::atomic<bool> ChatTaskManager::running;
std::thread ChatTaskManager::timer;


//////////////
// 总的任务管理器
//////////////
class TaskManager{
private:
    std::vector<std::unique_ptr<BaseTaskManager>> tsk_managers;
    void registerTaskManager(std::unique_ptr<BaseTaskManager> tsk_manager)
    {
        tsk_managers.emplace_back(std::move(tsk_manager));
    }

public:
    TaskManager()
    {
        // 注册特定任务管理器
        registerTaskManager(std::make_unique<CmdTaskManager>());
        registerTaskManager(std::make_unique<ChatTaskManager>()); // 注意这里AI任务优先级低于命令任务
        registerTaskManager(std::make_unique<JsonTaskManager>());
    }
    // 总的任务处理函数
    json handleTask(const MessageContext& msgctx)
    {
        for(auto& tsk_manager : tsk_managers)
        {
            if(tsk_manager->canHandle(msgctx))
            {
                // 按照能否处理自动分发任务处理器
                return tsk_manager->handleTask(msgctx);
            }
        }
        return {};
    }
};

////////////
// 总管理器
////////////
class Manager{
private:
    // 创建服务器
    httplib::Server svr;
    
public:
    // 创建任务管理器
    TaskManager tsk_manager;

    Manager()
    {
        // 注册 POST 路由
        svr.Post("/", [this](const httplib::Request& req, httplib::Response& res){
            this->handlePost(req, res);
        });
        // 创建 cache 目录
        std::filesystem::path cache_dir = CACHE_PATH;
        std::filesystem::create_directories(cache_dir); // 这个函数是原子操作
        Logger::info("cache目录: ", cache_dir);
        // 创建 data 目录
        std::filesystem::path data_dir = DATA_PATH;
        std::filesystem::create_directories(data_dir); // 这个函数是原子操作
        Logger::info("data目录: ", data_dir);
    }

    // 启动服务器
    void start()
    {
        svr.listen(CLIENT_HOST, CLIENT_PORT);
    }

    // 处理 POST 请求
    void handlePost(const httplib::Request& req, httplib::Response& res)
    {
        // 先解析JSON
        json data;
        try
        {
            data = json::parse(req.body);
        }catch(const json::parse_error& e) {
            Logger::warn("接受 QQ 消息 JSON 解析失败: ", e.what());
            res.status = 400;
            res.set_content("{}", "application/json");
            return;
        }
        //只处理消息事件, 其余事件todo
        if (!data.contains("post_type") || data["post_type"] != "message")
        {
            res.set_content("{}", "text/plain");
            return;
        }
        // 构造 MessageContext
        MessageContext msgctx;
        msgctx = MessageManager::getMessageContext(data);
        Logger::info("获取消息结构成功", msgctx.msg_segments);
        // 调用任务管理器 得到回复
        json reply = tsk_manager.handleTask(msgctx);
        Logger::info("回复内容: ", reply);
        if (!reply.empty())
        {
            MessageManager::send_msg(msgctx, reply);
        }
        res.set_content("{}", "text/plain");
    }
};

int main()
{
    Logger::info(" === 加载配置", " === ");
    load_config();
    Logger::info(" === 创建管理器", " === ");
    Manager m;
    Logger::info(" === 开始监听", " === ");
    m.start();
    return 0;
}
// g++ src/main.cpp src/config.cpp -O2 -Iinclude -o release/bot -lssl -lcrypto -lpthread