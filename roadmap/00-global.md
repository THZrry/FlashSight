# P0 全局（基础设施）

> 前置：无。产出：能编译的空架子 + `fs_tests` 空跑通过。

## 仓库与构建

- [ ] git 仓库初始化（按 rules-for-LLMs §Setup：用户指令为准）。
- [ ] xmake 四 targets 搭架子：`fs_core` / `fs_ui` / `flashsight` / `fs_tests`（见 arch.md §3）。
- [ ] 目录骨架：`src/{core,agent,tools,provider,storage,common,ui}` + `tests/` + `3rdparty/`。
- [ ] `temp/environment.md`：记录 python、C++ 编译器版本、xmake 版本、系统 shell。
- [ ] C++14 约束检查：禁 `std::filesystem`（用 ghsl），CI 或编译脚本里可加 grep 门。

## 第三方引入（3rdparty）

- [ ] ghsl filesystem 接入 xmake（C++14 可用验证）。
- [ ] nlohmann/json 接入。
- [ ] dtl 接入。
- [ ] md4c 接入（先原生，魔改 diff AST 留到 P4 渲染联调）。
- [ ] cpp-httplib + mbedTLS 编译验证（Win + Linux，HTTPS 真请求一次）→ 不可用则回退 libcurl。
- [ ] libssh2 预留（v1 不用，只保证能链接或明确缺席不阻塞编译）。
- [ ] reproc 接入（终端子系统在 P1 用，P0 先保证链接通过）。

## common 基础设施（src/common）

- [ ] 错误码体系（含拒绝类 `{allowed:false, reason, lockedBy?, howToProceed}`）。
- [ ] 日志分级（debug/info/warn/error；终端输出与流式输出不走日志，走事件）。
- [ ] 可注入时钟（单测用，终端 5min 晋升/唤醒定时器依赖它）。
- [ ] 可桩网络层（单测不碰真网）。
- [ ] 路径工具（基于 ghsl：join/normalize/expandUser，Win `%USERPROFILE%` 与 Linux `~` 统一）。
- [ ] i18n 基础设施：语言文件格式定死（编译时预处理进 cpp 的流程先跑通，中英两份空表）。

## 验收

- [ ] `xmake build` 全 targets 通过（Win + Linux 至少其一，另一个记 TODO）。
- [ ] `xmake test`（fs_tests 空跑）通过。
