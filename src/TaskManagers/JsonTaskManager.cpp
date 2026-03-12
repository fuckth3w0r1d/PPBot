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
    bvinfo.face = data["pages"][0]["first_frame"].get<std::string>();
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
json JsonTaskManager::handleBV(const json& data)
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
    std::string result_text;
    result_text += "📺 视频信息\n";
    result_text += "────────────────\n";
    result_text += "🎬 标题: " + bvinfo.title + "\n";
    result_text += "👤 UP主: " + bvinfo.up + "\n";
    result_text += "🆔 BVID: " + bvinfo.bvid + "\n";
    result_text += "────────────────\n";
    result_text += "👀 播放量: " + std::to_string(bvinfo.view) + "\n";
    result_text += "💬 评论: " + std::to_string(bvinfo.reply) + "\n";
    result_text += "⭐ 收藏: " + std::to_string(bvinfo.favorite) + "\n";
    result_text += "💰 投币: " + std::to_string(bvinfo.coin) + "\n";
    result_text += "🔗 分享: " + std::to_string(bvinfo.share) + "\n";
    result_text += "👍 点赞: " + std::to_string(bvinfo.like) + "\n";
    // 下载B站视频
    httplib::Headers headers = {
        {"Referer", "https://www.bilibili.com"},
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
    };
    // 先清理缓存
    FileManager::cleanCache();
    std::string video_path = FileManager::downloadFile(bvinfo.url, headers, CACHE_PATH, bvinfo.bvid + ".mp4");
    // 构造消息段
    json result = json::array();
    result.emplace_back(MessageManager::buildMsg("image", bvinfo.face));
    result.emplace_back(MessageManager::buildMsg("text", result_text));
    if(video_path.empty())
    {
        result.emplace_back(MessageManager::buildMsg("text", "视频下载异常, 可能是视频太大了"));
    }else{
        result.emplace_back(MessageManager::buildMsg("video", "file://" + video_path));
    }
    return result;
}

bool JsonTaskManager::canHandle(const MessageContext& msgctx)
{
    // 当群聊消息类型为 json 时能处理
    return (msgctx.msg_type == "group") && msgctx.pmsgsegs.has_json;
}
std::pair<json, std::string> JsonTaskManager::handleTask(const MessageContext& msgctx)
{
    const json& data = msgctx.pmsgsegs.json_data;
    if(data.contains("meta"))
    {
        if(data["meta"].contains("detail_1"))
        {
            if(data["meta"]["detail_1"]["appid"].get<std::string>() == B23_APPID)
            { // 暂时只处理B站分享视频
                auto result = handleBV(data);
                return std::make_pair(result, "direct");
            }
        }
    }
    Logger::warn("json 消息内容不符合预期", data);
    return {};
}
