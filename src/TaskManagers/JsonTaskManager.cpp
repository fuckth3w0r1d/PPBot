#include "TaskManagers/TaskManagers.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

// 获取 bvid
std::string JsonTaskManager::getBVid(const json& data)
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
JsonTaskManager::BVinfo JsonTaskManager::getBVinfo(const json& raw_data)
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
void JsonTaskManager::getBVUrlandSize(const std::string& bvid, const std::string& cid, BVinfo& bvinfo)
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
std::pair<std::string, std::string> JsonTaskManager::handleBV(const json& data)
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

bool JsonTaskManager::canHandle(const MessageContext& msgctx)
{
    // 当群聊消息类型为 json 时能处理
    return (msgctx.msg_type == "group") && msgctx.pmsgsegs.has_json;
}
json JsonTaskManager::handleTask(const MessageContext& msgctx)
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
