#include "ToolManagers/FileManager.h"
#include "logger.h"
#include "config.h"
#include <cerrno>
#include <cstring>

using json = nlohmann::json;

//////////
// 文件管理器
///////////

// 定义文件锁表
std::unordered_map<std::string, std::shared_ptr<std::mutex>> FileManager::file_mutex_map;
std::mutex FileManager::file_mutex_map_lock;
 
// 获取文件锁
std::shared_ptr<std::mutex> FileManager::getFileMutex(const std::string& path)
{
    std::lock_guard<std::mutex> lock(file_mutex_map_lock); // 文件锁表上锁
    if(file_mutex_map.find(path) == file_mutex_map.end())
    {
        file_mutex_map[path] = std::make_shared<std::mutex>();
    }
    return file_mutex_map[path];
}

bool FileManager::isValidFilename(const std::string& filename)
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

// 缓存清理函数
void FileManager::cleanCache()
{
    try
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
            return;
        }
        // 按最后修改时间排序（最大最旧的在前）
        std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b){
                auto size_a = std::filesystem::file_size(a);
                auto size_b = std::filesystem::file_size(b);
                if (size_a != size_b) return size_a > size_b;  // 大的在前
                else return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b); // 旧的在前
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
                Logger::debug("缓存文件已删除: ", files[i].filename().string());
                // 从锁表中移除
                std::lock_guard<std::mutex> lock(file_mutex_map_lock);
                file_mutex_map.erase(files[i].generic_string());
            }
            else
            {
                Logger::warn("缓存文件删除失败: ", files[i].string() + " reason=" + ec.message());
            }
        }
    }
    catch(const std::filesystem::filesystem_error& error)
    {
        Logger::warn("缓存清理失败: ", error.what());
    }
}

// 网络文件下载到指定目录
std::string FileManager::downloadFile(const std::string& url, const httplib::Headers& headers, const std::string& path, const std::string& filename)
{
    try
    {
        if(!std::filesystem::exists(path))
        {
            Logger::warn("文件下载失败: ", "目录不存在 path=" + path);
            return "";
        }
        if(!isValidFilename(filename)) return "";
        const std::regex pattern(R"(https?://([^/:]+)(:\d+)?(/.*))");
        std::smatch match;
        if(!std::regex_match(url, match, pattern))
        {
            Logger::warn("文件下载失败: ", "URL 格式无效");
            return "";
        }
        const auto save_path = std::filesystem::path(path) / filename;
        const auto file_mutex = getFileMutex(save_path.generic_string());
        std::lock_guard<std::mutex> file_lock(*file_mutex);
        if(std::filesystem::exists(save_path))
        {
            Logger::debug("复用已下载文件: ", save_path);
            return save_path.generic_string();
        }
        httplib::SSLClient cli(match[1].str());
        cli.set_follow_location(true);
        cli.enable_server_certificate_verification(false);
        cli.set_read_timeout(60);
        cli.set_write_timeout(60);
        errno = 0;
        std::ofstream output(save_path, std::ios::binary);
        if(!output)
        {
            Logger::error("下载文件创建失败: ", save_path.string() + " reason=" + (errno ? std::strerror(errno) : "文件流打开失败"));
            return "";
        }
        std::unique_ptr<char[]> buffer(new char[DOWNLOAD_BUFFER_SIZE]);
        output.rdbuf()->pubsetbuf(buffer.get(), DOWNLOAD_BUFFER_SIZE);
        size_t downloaded = 0;
        bool size_limit_exceeded = false;
        bool write_failed = false;
        std::string write_reason;
        int status = 0;
        std::string error_body;
        auto response = cli.Get(match[3].str(), headers,
            [&](const httplib::Response& result) {
                status = result.status;
                return true;
            },
            [&](const char* data, size_t length) {
                if(status != 200)
                {
                    if(status >= 300 && status < 400) return true;
                    error_body.append(data, std::min(length, size_t{2048} - error_body.size()));
                    return error_body.size() < 2048;
                }
                if(length > DOWNLOAD_SIZE_LIMIT - std::min(downloaded, DOWNLOAD_SIZE_LIMIT))
                {
                    size_limit_exceeded = true;
                    return false;
                }
                errno = 0;
                output.write(data, length);
                if(!output)
                {
                    write_failed = true;
                    write_reason = errno ? std::strerror(errno) : "文件流写入失败";
                    return false;
                }
                downloaded += length;
                return true;
            });
        errno = 0;
        output.close();
        if(!output && !write_failed)
        {
            write_failed = true;
            write_reason = errno ? std::strerror(errno) : "文件流关闭失败";
        }
        if(size_limit_exceeded)
            Logger::warn("文件下载失败: ", save_path.string() + " reason=超过大小限制 limit=" + std::to_string(DOWNLOAD_SIZE_LIMIT));
        else if(write_failed)
            Logger::error("下载文件写入失败: ", save_path.string() + " reason=" + write_reason);
        else if(status && status != 200)
            Logger::httpError("文件下载", status, error_body.empty() && response ? response->body : error_body);
        else if(!response)
            Logger::warn("文件下载 网络请求失败: ", httplib::to_string(response.error()));
        else
        {
            Logger::debug("文件下载完成: ", save_path.string() + " bytes=" + std::to_string(downloaded));
            return save_path.generic_string();
        }
        std::error_code cleanup_error;
        std::filesystem::remove(save_path, cleanup_error);
        if(cleanup_error) Logger::warn("失败下载文件清理失败: ", save_path.string() + " reason=" + cleanup_error.message());
        return "";
    }
    catch(const std::filesystem::filesystem_error& error)
    {
        Logger::warn("文件下载路径错误: ", error.what());
        return "";
    }
}


// 写入 json 数据文件
bool FileManager::writeJsonFile(const std::string& path, const json& data)
{
    const auto file_mutex = getFileMutex(path);
    std::lock_guard<std::mutex> lock(*file_mutex);
    std::string content;
    try
    {
        content = data.dump(4);
    }
    catch(const json::exception& error)
    {
        Logger::error("JSON 文件序列化失败: ", path + " reason=" + error.what());
        return false;
    }
    errno = 0;
    std::ofstream output(path);
    if(!output)
    {
        Logger::error("JSON 文件打开失败: ", path + " reason=" + (errno ? std::strerror(errno) : "文件流打开失败"));
        return false;
    }
    errno = 0;
    output << content;
    output.close();
    if(!output)
    {
        Logger::error("JSON 文件写入失败: ", path + " reason=" + (errno ? std::strerror(errno) : "文件流写入失败"));
        return false;
    }
    return true;
}

bool FileManager::readJsonFile(const std::string& path, json& data)
{
    const auto file_mutex = getFileMutex(path);
    std::lock_guard<std::mutex> lock(*file_mutex);
    errno = 0;
    std::ifstream input(path);
    if(!input)
    {
        Logger::warn("JSON 文件打开失败: ", path + " reason=" + (errno ? std::strerror(errno) : "文件流打开失败"));
        return false;
    }
    try
    {
        data = json::parse(input);
    }
    catch(const json::exception& error)
    {
        Logger::warn("JSON 文件解析失败: ", path + " reason=" + error.what());
        return false;
    }
    if(input.bad())
    {
        Logger::warn("JSON 文件读取失败: ", path + " reason=" + (errno ? std::strerror(errno) : "文件流读取失败"));
        return false;
    }
    return true;
}
