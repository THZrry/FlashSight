// ui_server.hpp - 本地 HTTP 服务器：提供内嵌 web 资产 + 后端 API。
//
// WhaleUI 未就位前的过渡方案：exe 起本地服务，浏览器打开即用（wasm UI）。
// 服务两类请求：
//   GET  /<asset>              内嵌静态资源（index.html/style.css/fs_ui.js/.wasm）
//   POST /api/call             调后端方法（UiApi -> FsApi）
//   GET  /api/events?since=N   拉取后端事件（自 N 之后）
// 事件来自 FsApi 的 event sink，存进环形缓冲；wasm 侧定时轮询。
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace httplib {
class Server;   // registered routes are re-attached on every rebind
}

namespace fs {

class FsApi;

namespace ui {

class UiServer {
 public:
  UiServer();
  ~UiServer();

  // 绑定 FsApi：注册 /api/call 分派与事件采集。须在 start 前调用。
  void bind_api(FsApi* api);

  // 监听地址 + 端口 + 访问密码；返回实际 URL（如 http://127.0.0.1:8123/）
  //
  // host = "127.0.0.1"（默认，仅本机）或 "0.0.0.0"（局域网/远程可达）。
  // password 非空时，非回环来源必须带 `Authorization: Bearer <password>`；
  // 回环（127.0.0.1 / ::1）始终免密，本机自己用不该被自己的密码挡住。
  FsResult start(int port = 0, const std::string& host = "127.0.0.1",
                 const std::string& password = "");
  void stop();
  bool running() const;
  std::string url() const;
  // 实际监听地址/端口/是否要求密码（设置页显示用）
  std::string host() const;
  int port() const;
  bool requires_password() const;
  // 本机在局域网里的 IP（best-effort）。监听 0.0.0.0 时设置页要显示一个可以
  // 直接照抄的地址；没有默认路由（离线）时返回空串。
  std::string lan_ip() const;
  // 是否正在重新绑定（设置页据此延迟刷新状态，避免读到旧端口）
  bool rebinding() const;
  // 上一次重绑失败的原因（空 = 没有失败）；设置页要显示出来
  std::string last_error() const;

  // 事件缓冲（测试可直接取；start 后由 FsApi sink 填充）
  Json drain_events(int64_t since, int64_t* next);

  // 用当前设置重新绑定：先 stop 再 start。返回新的监听结果。
  //
  // 设置页改"监听地址/端口/密码"要立即生效，而 httplib 的 Server 不能在
  // listen 之后换绑定地址，所以只能整体重启。正在进行的 SSE 连接会被断开，
  // 浏览器会自动重连（EventSource 内建行为）。
  FsResult restart(int port, const std::string& host, const std::string& password);

  // 密码校验 + 会话令牌。远程来源第一次访问要先 POST /api/login 换一个令牌，
  // 之后所有请求带 `Authorization: Bearer <token>`。令牌只存在内存里，重启即失效。
  bool check_password(const std::string& password) const;
  std::string issue_token();
  bool valid_token(const std::string& token) const;
  bool auth_enabled() const { return requires_password(); }
  // 这个连接是否来自本机（页面用 /api/auth 的返回决定要不要弹密码层）
  static bool is_loopback(const std::string& addr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  // 在新建的 Server 上注册全部路由 + 鉴权门。重绑会换一个实例，所以注册
  // 必须能重复执行（httplib 的 Server 不能改绑定地址）。
  void configure(httplib::Server& svr);
  // 绑定监听 socket（不开始 accept）。port 0 = 系统分配；actual 回填实际端口。
  FsResult bind_server(httplib::Server& svr, int port, int* actual);
};

}  // namespace ui
}  // namespace fs
