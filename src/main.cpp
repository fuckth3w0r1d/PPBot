#include <iostream>      
#include <string>        
#include <vector>        
#include <memory>        
#include <unordered_map> 
#include <ctime>         
#include <cstddef>
#include <regex>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <shared_mutex>  
#include <mutex>    
#include <chrono>    
#include <thread> 

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"

#include "config.h"
#include "logger.h"
#include "ToolManagers/MessageManager.h"
#include "ToolManagers/FileManager.h"
#include "TaskManagers/TaskManagers.h"
#include "BotServer.h"
#include "TaskManagers/Commands.h"

using json = nlohmann::json;


int main()
{
    Logger::info(" === 加载配置", " === ");
    load_config();
    Logger::info(" === 创建管理器", " === ");
    BotServer bot;
    Logger::info(" === 开始监听", " === ");
    bot.start();
    return 0;
}
