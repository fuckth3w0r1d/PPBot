#include "BotServer.h"
#include "logger.h"
#include "config.h"
#include <stdexcept>

BotServer::BotServer()
{
    // 注册 POST 路由
    svr.Post("/", [this](const httplib::Request& req, httplib::Response& res){
        this->handlePost(req, res);
    });
    // 创建 cache 目录
    std::filesystem::path cache_dir = CACHE_PATH;
    std::filesystem::create_directories(cache_dir); // 这个函数是原子操作
    // 创建 data 目录
    std::filesystem::path data_dir = DATA_PATH;
    std::filesystem::create_directories(data_dir); // 这个函数是原子操作
    Logger::info("运行目录: ", json{{"cache", CACHE_PATH}, {"data", DATA_PATH}});
}

// 启动服务器
void BotServer::start()
{
    Logger::info("HTTP 服务启动: ", CLIENT_HOST + ":" + std::to_string(CLIENT_PORT));
    if(!svr.listen(CLIENT_HOST, CLIENT_PORT))
        throw std::runtime_error("HTTP 监听失败 host=" + CLIENT_HOST + " port=" + std::to_string(CLIENT_PORT));
}

// 处理 POST 请求
void BotServer::handlePost(const httplib::Request& req, httplib::Response& res)
{
    // 先解析JSON
    json data;
    try
    {
        data = json::parse(req.body);
    }catch(const json::parse_error& e) {
        Logger::warn("QQ 上报 JSON 解析失败: ", e.what());
        res.status = 400;
        res.set_content("{}", "application/json");
        return;
    }
    if ((!data.contains("post_type") || data["post_type"] != "message") && !MessageManager::isGroupPoke(data))
    {
        res.set_content("{}", "text/plain");
        return;
    }
    // 构造 MessageContext
    MessageContext msgctx;
    try
    {
        msgctx = MessageManager::getMessageContext(data);
    }
    catch(const std::exception& e)
    {
        Logger::warn("QQ 消息字段无效: ", e.what());
        res.status = 400;
        res.set_content("{}", "application/json");
        return;
    }
    Logger::info("收到消息: ", json{{"group", msgctx.group_id}, {"user", msgctx.user_id},
                                  {"message", msgctx.message_id}, {"type", msgctx.is_poke ? "poke" : msgctx.msg_type}});
    Logger::debug("消息内容: ", msgctx.msg_segments);
    try
    {
        auto [reply, sendType] = tsk_manager.handleTask(msgctx);
        if (!reply.empty())
        {
            Logger::debug("待发送回复: ", reply);
            for(const auto& sent : MessageManager::send_msg(msgctx, reply, sendType))
                tsk_manager.recordOutgoing(msgctx, sent);
        }
    }
    catch(const std::exception& error)
    {
        Logger::error("消息处理失败: ", json{{"group", msgctx.group_id}, {"message", msgctx.message_id},
                                          {"reason", error.what()}});
        res.status = 500;
    }
    res.set_content("{}", "text/plain");
}
