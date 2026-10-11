// ui_backend.hpp - UI 后端抽象（桌面原生 vs 浏览器 wasm 跑同一套 C++ 逻辑）
// 桌面原生：进程内函数调用（NativeBackend 直接调 FsApi，不走 HTTP）。
// 浏览器：wasm 里的逻辑经 BrowserDom + glue.js 操作 DOM；以后需要远端时再加 HttpBackend。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fs_error.hpp"
#include "fs_json.hpp"
#include "ui_dom.hpp"

namespace fs {
namespace ui {

// 后端调用：方法名 + 参数 -> 结果（同步；流式经事件回调）
using BackendCall = std::function<FsResult(const std::string& method, const Json& args)>;
// 事件订阅：后端推事件（stream.delta / turn.done / wake.fired ...）
using BackendEvent = std::function<void(const std::string& event, const Json& payload)>;

class IUiBackend {
 public:
  virtual ~IUiBackend() = default;
  // 同步调用后端方法（如 session.send / provider.listModels）
  virtual FsResult call(const std::string& method, const Json& args) = 0;
  // 订阅后端事件；返回退订函数
  virtual std::function<void()> subscribe(BackendEvent fn) = 0;
};

// 空后端（UI 单测 / 原型演示用：call 返回空成功，事件可手动触发）
class NullBackend : public IUiBackend {
 public:
  FsResult call(const std::string& method, const Json& args) override;
  std::function<void()> subscribe(BackendEvent fn) override;
  // 测试手动推事件
  void fire(const std::string& event, const Json& payload);

 private:
  std::vector<BackendEvent> subs_;
};

}  // namespace ui
}  // namespace fs