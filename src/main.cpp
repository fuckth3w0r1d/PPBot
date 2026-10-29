#include "BotServer.h"
#include "config.h"
#include "logger.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <stdexcept>
#include <thread>

int main()
{
    try
    {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        const int mask_error = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        if(mask_error) throw std::runtime_error(std::string("无法阻塞停机信号: ") + std::strerror(mask_error));
        load_config();
        Logger::info("配置加载完成", "");
        BotServer bot;
        std::atomic<bool> finished{false};
        std::thread signal_thread([&]{
            while(!finished)
            {
                const timespec timeout{0, 100000000};
                const int signal = sigtimedwait(&signals, nullptr, &timeout);
                if(signal == SIGINT || signal == SIGTERM)
                {
                    Logger::info("收到正常停机信号: ", signal);
                    bot.stop();
                    return;
                }
                if(signal < 0 && errno != EAGAIN && errno != EINTR)
                {
                    Logger::error("停机信号监听失败: ", std::strerror(errno));
                    bot.stop();
                    return;
                }
            }
        });
        try
        {
            bot.start();
        }
        catch(...)
        {
            finished = true;
            signal_thread.join();
            throw;
        }
        finished = true;
        signal_thread.join();
        return bot.tsk_manager.shutdown() ? 0 : 1;
    }
    catch(const std::exception& error)
    {
        Logger::error("启动失败: ", error.what());
        return 1;
    }
}
