#include "BotServer.h"
#include "config.h"
#include "logger.h"

int main()
{
    try
    {
        load_config();
        Logger::info("配置加载完成", "");
        BotServer bot;
        bot.start();
        return 0;
    }
    catch(const std::exception& error)
    {
        Logger::error("启动失败: ", error.what());
        return 1;
    }
}
