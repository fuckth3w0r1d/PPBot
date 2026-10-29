#include "TaskManagers/TaskManagers.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

// 获取 bvid
std::string JsonTaskManager::getBVid(const json& data)
{
    std::string url;
    try
    {
        url = data.at("meta").at("detail_1").at("qqdocurl").get<std::string>();
    }
    catch(const json::exception& error)
    {
        Logger::warn("B站卡片解析失败: ", error.what());
        return "";
    }
    const std::regex pattern(R"(https?://([^/:]+)(:\d+)?(/.*))");
    std::smatch match;
    if(!std::regex_match(url, match, pattern))
    {
        Logger::warn("B站短链解析失败: ", "URL 格式无效");
        return "";
    }
    httplib::SSLClient cli(match[1].str(), 443);
    auto res = cli.Head(match[3].str());
    if(!res)
    {
        Logger::warn("B站短链 网络请求失败: ", httplib::to_string(res.error()));
        return "";
    }
    if(res->status != 302)
    {
        Logger::httpError("B站短链", res->status, res->body, "预期 HTTP 302 重定向");
        return "";
    }
    const auto location = res->get_header_value("Location");
    const std::regex bv_pattern(R"(BV[A-Za-z0-9]{10})");
    std::smatch bv_match;
    if(std::regex_search(location, bv_match, bv_pattern)) return bv_match[0];
    Logger::warn("B站短链解析失败: ", "重定向地址未包含 BV 号");
    return "";
}
// 获取B站视频信息
JsonTaskManager::BVinfo JsonTaskManager::getBVinfo(const json& raw_data)
{
    BVinfo bvinfo{};
    const auto& data = raw_data.at("data");
    bvinfo.cid = std::to_string(data.at("cid").get<size_t>());
    bvinfo.bvid = data.at("bvid").get<std::string>();
    bvinfo.title = data.at("title").get<std::string>();
    bvinfo.up = data.at("owner").at("name").get<std::string>();
    bvinfo.face = data.at("pages").at(0).at("first_frame").get<std::string>();
    bvinfo.view = data.at("stat").at("view").get<int>();
    bvinfo.reply = data.at("stat").at("reply").get<int>();
    bvinfo.favorite = data.at("stat").at("favorite").get<int>();
    bvinfo.coin = data.at("stat").at("coin").get<int>();
    bvinfo.share = data.at("stat").at("share").get<int>();
    bvinfo.like = data.at("stat").at("like").get<int>();
    getBVUrlandSize(bvinfo.bvid, bvinfo.cid, bvinfo);
    return bvinfo;
}
// 获取B站视频直链url和视频大小
void JsonTaskManager::getBVUrlandSize(const std::string& bvid, const std::string& cid, BVinfo& bvinfo)
{
    httplib::SSLClient cli(B23_HOST, B23_PORT);
    auto res = cli.Get(B23_GET_PLAY_PATH + "?bvid=" + bvid + "&cid=" + cid);
    if(!res)
    {
        Logger::warn("B站播放地址 网络请求失败: ", httplib::to_string(res.error()));
        return;
    }
    if(res->status != 200)
    {
        Logger::httpError("B站播放地址", res->status, res->body);
        return;
    }
    try
    {
        const auto data = json::parse(res->body);
        if(data.at("code").get<int>() != 0 || data.at("data").at("durl").empty())
        {
            Logger::httpError("B站播放地址", res->status, res->body, "接口未提供播放地址");
            return;
        }
        const auto& video = data.at("data").at("durl").at(0);
        const auto url = video.at("url").get<std::string>();
        const auto size = video.at("size").get<size_t>();
        if(url.empty())
        {
            Logger::httpError("B站播放地址", res->status, res->body, "响应包含空播放地址");
            return;
        }
        bvinfo.url = url;
        bvinfo.size = size;
    }
    catch(const json::exception& error)
    {
        Logger::httpError("B站播放地址", res->status, res->body, std::string("响应解析失败: ") + error.what());
    }
}

// 处理B站视频
json JsonTaskManager::handleBV(const json& data)
{
    const auto bvid = getBVid(data);
    if(bvid.empty()) return json::array({MessageManager::buildMsg("text", "B站视频链接解析失败")});
    httplib::SSLClient cli(B23_HOST, B23_PORT);
    auto res = cli.Get(B23_GET_QUERY_PATH + "?bvid=" + bvid);
    if(!res)
    {
        Logger::warn("B站视频信息 网络请求失败: ", httplib::to_string(res.error()));
        return json::array({MessageManager::buildMsg("text", "B站视频信息获取失败")});
    }
    if(res->status != 200)
    {
        Logger::httpError("B站视频信息", res->status, res->body);
        return json::array({MessageManager::buildMsg("text", "B站视频信息获取失败")});
    }
    BVinfo bvinfo{};
    try
    {
        const auto raw_data = json::parse(res->body);
        if(raw_data.at("code").get<int>() != 0)
        {
            Logger::httpError("B站视频信息", res->status, res->body, "接口业务失败");
            return json::array({MessageManager::buildMsg("text", "B站视频信息获取失败")});
        }
        bvinfo = getBVinfo(raw_data);
    }
    catch(const json::exception& error)
    {
        Logger::httpError("B站视频信息", res->status, res->body, std::string("响应解析失败: ") + error.what());
        return json::array({MessageManager::buildMsg("text", "B站视频信息获取失败")});
    }
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
    std::string video_path;
    if(!bvinfo.url.empty())
    {
        FileManager::cleanCache();
        video_path = FileManager::downloadFile(bvinfo.url, headers, CACHE_PATH, bvinfo.bvid + ".mp4");
    }
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
    if(msgctx.msg_type != "group" || !msgctx.pmsgsegs.has_json) return false;
    const auto& data = msgctx.pmsgsegs.json_data;
    return data.is_object() && data.contains("meta") && data["meta"].is_object() &&
        data["meta"].contains("detail_1") && data["meta"]["detail_1"].is_object() &&
        data["meta"]["detail_1"].contains("appid") && data["meta"]["detail_1"]["appid"] == B23_APPID;
}
std::pair<json, std::string> JsonTaskManager::handleTask(const MessageContext& msgctx)
{
    if(!canHandle(msgctx)) return {};
    return {handleBV(msgctx.pmsgsegs.json_data), "direct"};
}
