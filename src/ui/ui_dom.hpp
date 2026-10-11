// ui_dom.hpp - DOM 兼容层（不预设 WhaleUI API）
// 设计：FlashSight UI 逻辑只依赖本接口。两个实现：
//   - NullDom：空实现（桌面版占位，等 WhaleUI 纯 C API 就绪后再接真实现）
//   - BrowserDom：wasm 分支，经极薄 JS 胶水操作浏览器 DOM（见 web/glue.js 说明）
// WhaleUI 就绪后加第三个实现 WhaleDom 即可，UI 逻辑不动。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs {
namespace ui {

// 不透明节点句柄（实现各自解释：WhaleUI 节点 / 空指针 / 浏览器端 id）
struct Node {
  uint64_t id = 0;
  bool valid() const { return id != 0; }
};

// 事件类型（子集，够原型用）
enum class EventKind {
  kClick,
  kInput,
  kChange,
  kKeyEnter,
};

using EventFn = std::function<void(const std::string& value)>;

// 抽象 DOM 接口
class IDom {
 public:
  virtual ~IDom() = default;

  // 建节点：tag + 可选 class + 可选文本
  virtual Node create(const std::string& tag, const std::string& cls,
                      const std::string& text) = 0;
  // 挂载
  virtual void append(Node parent, Node child) = 0;
  virtual void remove(Node node) = 0;
  virtual void clear(Node parent) = 0;
  // 属性/文本/显隐
  virtual void set_text(Node node, const std::string& text) = 0;
  virtual void set_attr(Node node, const std::string& key, const std::string& value) = 0;
  virtual void set_visible(Node node, bool visible) = 0;
  // 事件
  virtual void on(Node node, EventKind kind, EventFn fn) = 0;
  // 根节点（页面容器）
  virtual Node root() = 0;
};

// 空实现：所有调用无操作（桌面版占位；单元测试也可用它断言调用序列）
class NullDom : public IDom {
 public:
  Node create(const std::string& tag, const std::string& cls,
              const std::string& text) override;
  void append(Node parent, Node child) override;
  void remove(Node node) override;
  void clear(Node parent) override;
  void set_text(Node node, const std::string& text) override;
  void set_attr(Node node, const std::string& key, const std::string& value) override;
  void set_visible(Node node, bool visible) override;
  void on(Node node, EventKind kind, EventFn fn) override;
  Node root() override { return Node{1}; }

  // 调用记录（测试用）
  struct Call {
    std::string method;
    uint64_t node = 0;
    std::string detail;
  };
  const std::vector<Call>& calls() const { return calls_; }
  void clear_calls() { calls_.clear(); }

 private:
  uint64_t next_id_ = 2;
  std::vector<Call> calls_;
};

}  // namespace ui
}  // namespace fs