// ui_widgets.hpp - Widget 库（前端框架思路，C++ 直写）
// 全部 Widget 经 IDom 渲染：浏览器（wasm+glue.js）与桌面原生跑同一套逻辑。
// 桌面原生走进程内函数调用（见 ui_backend.hpp IUiBackend），不走 HTTP。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui_dom.hpp"
#include "ui_reactive.hpp"

namespace fs {
namespace ui {

// ---------- 基类 ----------
class Widget {
 public:
  Widget(IDom& dom, const std::string& tag, const std::string& cls);
  virtual ~Widget() = default;
  Widget(const Widget&) = delete;
  Widget& operator=(const Widget&) = delete;

  Node node() const { return node_; }
  // 挂到父节点（返回自身链式调用）
  Widget& mount(Node parent);

 protected:
  IDom& dom_;
  Node node_{0};
};

// ---------- 展示类 ----------

// Text：静态/绑定文本
class Text : public Widget {
 public:
  Text(IDom& dom, const std::string& text, const std::string& cls = "");
  Text(IDom& dom, const std::shared_ptr<Signal<std::string>>& sig,
       const std::string& cls = "");
 private:
  Subscription binding_;
};

// CodeBlock：代码块（预留高亮，v1 纯文本 + mono class）
class CodeBlock : public Widget {
 public:
  CodeBlock(IDom& dom, const std::string& code, const std::string& lang = "");
  void set_code(const std::string& code);
};

// Badge：小徽标（计数/状态）
class Badge : public Widget {
 public:
  Badge(IDom& dom, const std::string& text);
  void set_text(const std::string& text);
};

// Progress：进度条（0..100，value 信号驱动）
class Progress : public Widget {
 public:
  Progress(IDom& dom, const std::shared_ptr<Signal<int>>& value);
 private:
  Subscription binding_;
  Node bar_{0};
};

// Spinner：加载中（显隐信号驱动）
class Spinner : public Widget {
 public:
  Spinner(IDom& dom, const std::shared_ptr<Signal<bool>>& visible);
 private:
  Subscription binding_;
};

// EmptyState：空态（图标+说明+可选操作按钮）
class EmptyState : public Widget {
 public:
  EmptyState(IDom& dom, const std::string& hint, const std::string& action_label = "");
  void on_action(EventFn fn);
 private:
  Node action_btn_{0};
};

// ---------- 输入类 ----------

using ChangeFn = std::function<void(const std::string&)>;

// TextInput：单行输入（value 双向绑定）
class TextInput : public Widget {
 public:
  TextInput(IDom& dom, const std::shared_ptr<Signal<std::string>>& value,
            const std::string& placeholder = "");
};

// TextArea：多行输入
class TextArea : public Widget {
 public:
  TextArea(IDom& dom, const std::shared_ptr<Signal<std::string>>& value,
           const std::string& placeholder = "");
};

// SearchBox：搜索框（输入即回调，防抖由调用方做）
class SearchBox : public Widget {
 public:
  SearchBox(IDom& dom, ChangeFn on_search, const std::string& placeholder = "搜索…");
};

// ---------- 选择类 ----------

// Button：按钮（含 primary/disabled 信号）
class Button : public Widget {
 public:
  Button(IDom& dom, const std::string& label, EventFn on_click,
         bool primary = false);
  Button(IDom& dom, const std::shared_ptr<Signal<std::string>>& label,
         EventFn on_click, bool primary = false);
  void set_disabled(bool disabled);
  void set_disabled_sig(const std::shared_ptr<Signal<bool>>& disabled);
 private:
  Subscription label_binding_;
  Subscription disabled_binding_;
};

// Select：下拉选择（options +选中下标信号）
class Select : public Widget {
 public:
  Select(IDom& dom, const std::vector<std::string>& options,
         const std::shared_ptr<Signal<int>>& selected);
 private:
  Subscription binding_;
};

// Checkbox：复选框
class Checkbox : public Widget {
 public:
  Checkbox(IDom& dom, const std::string& label,
           const std::shared_ptr<Signal<bool>>& checked);
 private:
  Subscription binding_;
};

// RadioGroup：单选组
class RadioGroup : public Widget {
 public:
  RadioGroup(IDom& dom, const std::vector<std::string>& options,
             const std::shared_ptr<Signal<int>>& selected);
};

// Toggle：开关
class Toggle : public Widget {
 public:
  Toggle(IDom& dom, const std::shared_ptr<Signal<bool>>& on);
 private:
  Subscription binding_;
};

// ---------- 容器/导航类 ----------

// Tabs：标签页（titles + 当前页信号 + 内容构建回调）
class Tabs : public Widget {
 public:
  using PageFn = std::function<void(IDom& dom, Node parent)>;
  Tabs(IDom& dom, const std::vector<std::string>& titles,
       const std::shared_ptr<Signal<int>>& current,
       const std::vector<PageFn>& pages);
  void rebuild();
 private:
  std::vector<std::string> titles_;
  std::shared_ptr<Signal<int>> current_;
  std::vector<PageFn> pages_;
  Node bar_{0};
  Node body_{0};
  Subscription binding_;
};

// Accordion：折叠面板（title + 展开信号 + 内容回调）
class Accordion : public Widget {
 public:
  using BodyFn = std::function<void(IDom& dom, Node parent)>;
  Accordion(IDom& dom, const std::string& title,
            const std::shared_ptr<Signal<bool>>& expanded, BodyFn body);
 private:
  void rebuild();
  std::string title_;
  std::shared_ptr<Signal<bool>> expanded_;
  BodyFn body_;
  Node head_{0};
  Node body_node_{0};
  Subscription binding_;
};

// ListView：虚拟化列表（一次 N 条 + 按需加载回调）
class ListView : public Widget {
 public:
  using RowFn = std::function<void(IDom& dom, Node parent, int index)>;
  ListView(IDom& dom, const std::shared_ptr<Signal<int>>& count, int page_size,
           RowFn row);
  void refresh();
 private:
  std::shared_ptr<Signal<int>> count_;
  int page_size_;
  RowFn row_;
  Node body_{0};
  Subscription binding_;
};

// Modal：弹窗（visible 信号 + 内容回调 + 关闭回调）
class Modal : public Widget {
 public:
  using BodyFn = std::function<void(IDom& dom, Node parent)>;
  Modal(IDom& dom, const std::shared_ptr<Signal<bool>>& visible,
        const std::string& title, BodyFn body, EventFn on_close);
 private:
  void rebuild();
  std::shared_ptr<Signal<bool>> visible_;
  std::string title_;
  BodyFn body_;
  EventFn on_close_;
  Subscription binding_;
};

// Tooltip：悬浮提示（title 属性，v1 用原生 title）
class Tooltip {
 public:
  static void attach(IDom& dom, Node node, const std::string& tip);
};

// Splitter：可拖拽分栏（左右两子节点 + 比例信号；拖拽 P4，v1 固定比例渲染）
class Splitter : public Widget {
 public:
  Splitter(IDom& dom, const std::shared_ptr<Signal<int>>& percent);
 private:
  Subscription binding_;
};

}  // namespace ui
}  // namespace fs