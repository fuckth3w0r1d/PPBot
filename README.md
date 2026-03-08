## 基于 NapCatQQ 搭建的简易 qq bot
使用 Cpp 编写的 qq 机器人, 实现了一些文本指令响应、`Json`卡片分享解析和AI对话功能。

### 环境说明
- Ubuntu 24.04  

- NapCat v4.17.29

- OpenSSL

  ```shell
  $ openssl version
  OpenSSL 3.0.13 30 Jan 2024 (Library: OpenSSL 3.0.13 30 Jan 2024)
  ```

- 编译器

  ```shell
  $ g++ --version
  g++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
  Copyright (C) 2023 Free Software Foundation, Inc.
  This is free software; see the source for copying conditions.  There is NO
  warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  ```

- 编译命令

  ```shell
  g++ src/main.cpp -O2 -Iinclude -o release/bot -lssl -lcrypto -lpthread
  ```

### 主要特性

- 使用 cpp-httplib 库，OneBot HTTP Server
- 总体模块化：模块低耦合，便于扩展和维护
- 消息处理模块：通过解析器将原始JSON解构为统一的 msgctx，支持链式调用
- 指令功能模块：实现 Command 类封装，便于派生实现更多指令，通过映射表分配到设定的指令
- 文件管理模块：运用 RAII 管理锁与缓存，线程安全。
- 分享JSON消息解析模块：针对特定卡片类型，提取元数据并触发下载
- AI对话模块：prompts 维护上下文，session 机制，用户画像，AI可变人格，定时保存json文件进行历史留存避免清空记忆
- 任务分发模块: 实现 BaseTaskManager 类封装，便于派生实现更多业务功能，遍历所有 TaskManager 自动分配任务

### 部署

参照 [NapCatQQ 使用文档](https://napneko.github.io/)

配置 `http` 客户端和 `http`上报服务端，收发消息格式配置为 `array`

