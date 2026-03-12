#include "BotServer.h"
#include "logger.h"
#include "config.h"

BotServer::BotServer()
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
void BotServer::start()
{
    svr.listen(CLIENT_HOST, CLIENT_PORT);
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
