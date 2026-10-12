// test_terminal.cpp - 终端子系统（reproc 封装）单测：真实起子进程、读输出、
// kill。跑的是本机 shell，不碰网络。
#include "test_framework.hpp"

#include "core/shell_detect.hpp"
#include "core/terminal_manager.hpp"
#include "core/terminal_registry.hpp"
#include "common/fs_clock.hpp"

using namespace fs;

FS_TEST(shell_detect_finds_something) {
  std::vector<ShellInfo> shells = ShellDetector::detect_all();
  // cmd 在 Windows 必可用；POSIX 上 bash/sh 至少有一个
  EXPECT_TRUE(!shells.empty());
  ShellInfo best = ShellDetector::resolve("", shells);
  EXPECT_TRUE(!best.name.empty());
}

FS_TEST(terminal_spawn_captures_output) {
  WallClock clock;
  TerminalRegistry reg(&clock);
  TerminalManager mgr(&reg, &clock);
  // 用 shell 跑一条打印命令（跨平台：echo 在 cmd/bash 都有）
  Json args = Json::object();
  args["command"] = "echo flashsight-term-test";
  args["background"] = false;
  args["timeoutMs"] = 15000;
  Json out;
  FsResult r = mgr.spawn(args, &out);
  ASSERT_TRUE(r.ok());
  std::string status = jsonutil::get<std::string>(out, "status", "");
  EXPECT_EQ(status, std::string("exited"));
  std::string output = jsonutil::get<std::string>(out, "output", "");
  EXPECT_TRUE(output.find("flashsight-term-test") != std::string::npos);
  // 注册表里应有一条已退出记录
  std::string tid = jsonutil::get<std::string>(out, "tid", "");
  EXPECT_TRUE(!tid.empty());
  const TermEntry* e = reg.find(tid);
  ASSERT_TRUE(e != nullptr);
  EXPECT_TRUE(e->status == TermStatus::kExited);
}

FS_TEST(terminal_poll_and_kill) {
  WallClock clock;
  TerminalRegistry reg(&clock);
  TerminalManager mgr(&reg, &clock);
  // 后台跑一个会持续一段时间的命令
#if defined(_WIN32)
  std::string cmd = "ping -n 6 127.0.0.1 > nul";
#else
  std::string cmd = "sleep 5";
#endif
  Json args = Json::object();
  args["command"] = cmd;
  args["background"] = true;
  args["timeoutMs"] = 20000;
  Json out;
  FsResult r = mgr.spawn(args, &out);
  ASSERT_TRUE(r.ok());
  std::string tid = jsonutil::get<std::string>(out, "tid", "");
  EXPECT_EQ(jsonutil::get<std::string>(out, "status", ""), std::string("running"));

  // poll：还在跑
  Json p;
  FsResult pr = mgr.poll(Json{{"tid", tid}}, &p);
  ASSERT_TRUE(pr.ok());
  EXPECT_EQ(jsonutil::get<std::string>(p, "status", ""), std::string("running"));

  // kill：应能终止
  Json k;
  FsResult kr = mgr.kill(Json{{"tid", tid}}, &k);
  ASSERT_TRUE(kr.ok());
  EXPECT_EQ(jsonutil::get<std::string>(k, "status", ""), std::string("killed"));
  const TermEntry* e = reg.find(tid);
  ASSERT_TRUE(e != nullptr);
  EXPECT_TRUE(e->status == TermStatus::kExited || e->status == TermStatus::kKilled);
}

FS_TEST(terminal_spawn_empty_command_errors) {
  WallClock clock;
  TerminalRegistry reg(&clock);
  TerminalManager mgr(&reg, &clock);
  Json args = Json::object();
  Json out;
  FsResult r = mgr.spawn(args, &out);
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.code == ErrCode::kInvalidArg);
}
