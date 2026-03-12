#include "ToolManagers/FileManager.h"
#include "logger.h"
#include "config.h"

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
std::string FileManager::downloadFile(const std::string& url, const httplib::Headers& headers, const std::string& path ,const std::string& filename)
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
bool FileManager::writeJsonFile(const std::string& path, const json& data)
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
bool FileManager::readJsonFile(const std::string& path, json& data)
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

