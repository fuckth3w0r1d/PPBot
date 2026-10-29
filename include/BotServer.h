#pragma once

#include <atomic>
#include "TaskManagers/TaskManagers.h"

////////////
// bot server
////////////
class BotServer{
private:
    // 创建服务器
    httplib::Server svr;
    std::atomic<bool> stopping{false};
    std::atomic<bool> start_finished{false};
    
public:
    // 创建任务管理器
    TaskManager tsk_manager;

    BotServer();

    // 启动服务器
    void start();
    void stop();

    // 处理 POST 请求
    void handlePost(const httplib::Request& req, httplib::Response& res);
};
