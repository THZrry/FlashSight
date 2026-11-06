// test_terminal.cpp - 终端子系统（reproc 封装）单测：真实起子进程、读输出、
// kill、交互式终端与输出编码。跑的是本机 shell，不碰网络。
#include "test_framework.hpp"

#include "core/shell_detect.hpp"
#include "core/terminal_manager.hpp"
#include "core/terminal_registry.hpp"
#include "common/fs_clock.hpp"
#include "common/fs_encoding.hpp"

using namespace fs;

#include <chrono>
#include <thread>

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

// A foreground command must NOT hold the manager lock while it waits: tick()
// (the wake scheduler's clock), other spawns and poll/kill all share it. The
// two-session freeze was `terminal_exec sleep 60` blocking every one of them for
// a minute.
FS_TEST(terminal_foreground_wait_does_not_block_the_manager) {
  WallClock clock;
  TerminalRegistry reg(&clock);
  TerminalManager mgr(&reg, &clock);
#if defined(_WIN32)
  std::string slow = "ping -n 5 127.0.0.1 > nul";   // ~4 s
#else
  std::string slow = "sleep 4";
#endif
  Json fg = Json::object();
  fg["command"] = slow;
  fg["background"] = false;
  fg["timeoutMs"] = 15000;
  Json fg_out;
  FsResult fg_r;
  std::thread t([&]() { fg_r = mgr.spawn(fg, &fg_out); });
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  mgr.tick();   // must not wait behind the foreground process
  Json bg = Json::object();
  bg["command"] = "echo quick";
  bg["background"] = true;
  Json bg_out;
  FsResult bg_r = mgr.spawn(bg, &bg_out);
  int64_t ms = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0).count());
  EXPECT_TRUE(bg_r.ok());
  EXPECT_TRUE(ms < 1500);
  t.join();
  EXPECT_TRUE(fg_r.ok());
  EXPECT_EQ(jsonutil::get<std::string>(fg_out, "status", ""), std::string("exited"));
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

// 交互式终端：没有"要执行的命令"，起的是 shell 本身；写一行回车它会执行并
// 把输出推出来（tick 只发新增字节）。
FS_TEST(terminal_interactive_write_and_tick) {
  WallClock clock;
  TerminalRegistry reg(&clock);
  TerminalManager mgr(&reg, &clock);
  Json args = Json::object();
  args["interactive"] = true;
  args["background"] = true;
  args["shell"] = "cmd";
  args["label"] = "交互测试";
  Json out;
  FsResult r = mgr.spawn(args, &out);
  if (!r.ok()) return;  // 非 Windows 或没有 cmd：跳过
  std::string tid = jsonutil::get<std::string>(out, "tid", "");
  EXPECT_EQ(jsonutil::get<std::string>(out, "status", ""), std::string("running"));
  EXPECT_EQ(jsonutil::get<std::string>(out, "label", ""), std::string("交互测试"));

  Json w;
  FsResult wr = mgr.write(Json{{"tid", tid}, {"text", "echo tick-marker\r\n"}}, &w);
  ASSERT_TRUE(wr.ok());
  EXPECT_TRUE(jsonutil::get_int(w, "written", 0) > 0);

  // 等命令执行：轮询 tick 直到出现标记（最多 10s）
  std::string all;
  bool found = false;
  for (int i = 0; i < 100 && !found; ++i) {
    std::vector<Json> evs = mgr.tick();
    for (size_t k = 0; k < evs.size(); ++k) {
      std::string chunk = jsonutil::get<std::string>(evs[k], "output", "");
      all += chunk;
      if (all.find("tick-marker") != std::string::npos) found = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(found);
  mgr.kill(Json{{"tid", tid}}, &out);
}

// 输出编码：中文 Windows 的 cmd 按 GBK 输出，解码后必须是合法 UTF-8 且内容
// 正确（这条曾经让 /api/events 直接 500：非法 UTF-8 进 JSON dump）。
FS_TEST(encoding_gbk_output_decodes) {
  // "版本" 的 GBK 字节
  std::string gbk = "\xB0\xE6\xB1\xBE";
  std::string pending;
  std::string utf8 = encoding::decode_output(gbk, "GBK", &pending);
  EXPECT_TRUE(utf8 == std::string("版本"));
  // 跨 read 边界：半个汉字要等下一块数据补齐，不能被切成两个替换符
  std::string half1 = "\xB0";
  std::string half2 = "\xE6\xB1\xBE";
  std::string p;
  std::string r1 = encoding::decode_output(half1, "GBK", &p);
  EXPECT_TRUE(r1.empty());
  EXPECT_EQ(p.size(), size_t(1));
  std::string r2 = encoding::decode_output(half2, "GBK", &p);
  EXPECT_TRUE(r2 == std::string("版本"));
  // 非法字节 -> 替换符，绝不产生非法 UTF-8
  std::string bad = "ok\xFF\xFE end";
  std::string clean = encoding::sanitize_utf8(bad);
  EXPECT_TRUE(clean.find("ok") == 0);
  // 已经是合法 UTF-8 的内容在"系统默认"下原样通过
  std::string utf8_in = "中文 abc";
  std::string same = encoding::decode_output(utf8_in, "", nullptr);
  EXPECT_TRUE(same == utf8_in);
}

// 写 stdin 的反向编码：ASCII 原样，中文按代码页转换（Windows 上 UTF-8 的
// 中文命令直接写管道会被 cmd 按 GBK 解析成乱码）。
FS_TEST(encoding_input_conversion) {
  std::string ascii = "echo hi\r\n";
  EXPECT_TRUE(encoding::encode_input(ascii, "") == ascii);
  std::string utf8_cn = "echo 中文";
  std::string enc = encoding::encode_input(utf8_cn, "GBK");
#if defined(_WIN32)
  // 中 = D6 D0, 文 = CE C4 in GBK
  std::string want = "echo \xD6\xD0\xCE\xC4";
  EXPECT_TRUE(enc == want);
#else
  EXPECT_TRUE(enc == utf8_cn);
#endif
}
