// ui_widgets_a.cpp - Widget 实现 part A（基类 + 展示类 + 输入类）
#include "ui_widgets.hpp"

namespace fs {
namespace ui {

// ---------- Widget ----------
Widget::Widget(IDom& dom, const std::string& tag, const std::string& cls)
    : dom_(dom) {
  node_ = dom_.create(tag, cls, "");
}

Widget& Widget::mount(Node parent) {
  dom_.append(parent, node_);
  return *this;
}

// ---------- Text ----------
Text::Text(IDom& dom, const std::string& text, const std::string& cls)
    : Widget(dom, "span", cls) {
  dom_.set_text(node_, text);
}

Text::Text(IDom& dom, const std::shared_ptr<Signal<std::string>>& sig,
           const std::string& cls)
    : Widget(dom, "span", cls) {
  binding_ = bind_text(dom_, node_, sig);
}

// ---------- CodeBlock ----------
CodeBlock::CodeBlock(IDom& dom, const std::string& code, const std::string& lang)
    : Widget(dom, "pre", "code-block") {
  (void)lang;  // v1 纯文本；高亮在 P4（设置-外观-代码高亮）
  dom_.set_text(node_, code);
}

void CodeBlock::set_code(const std::string& code) {
  dom_.set_text(node_, code);
}

// ---------- Badge ----------
Badge::Badge(IDom& dom, const std::string& text) : Widget(dom, "span", "badge") {
  dom_.set_text(node_, text);
}

void Badge::set_text(const std::string& text) {
  dom_.set_text(node_, text);
}

// ---------- Progress ----------
Progress::Progress(IDom& dom, const std::shared_ptr<Signal<int>>& value)
    : Widget(dom, "div", "progress") {
  bar_ = dom_.create("div", "progress-bar", "");
  dom_.append(node_, bar_);
  auto v = value;
  IDom* d = &dom_;
  Node b = bar_;
  binding_ = value->subscribe([d, b, v](const int&) {
    int pct = v->get();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    d->set_attr(b, "style", "width:" + std::to_string(pct) + "%");
  });
  int pct = value->get();
  dom_.set_attr(bar_, "style", "width:" + std::to_string(pct) + "%");
}

// ---------- Spinner ----------
Spinner::Spinner(IDom& dom, const std::shared_ptr<Signal<bool>>& visible)
    : Widget(dom, "span", "spinner") {
  dom_.set_text(node_, "...");
  binding_ = bind_visible(dom_, node_, visible);
}

// ---------- EmptyState ----------
EmptyState::EmptyState(IDom& dom, const std::string& hint,
                       const std::string& action_label)
    : Widget(dom, "div", "empty-state") {
  Node t = dom_.create("div", "dim", hint);
  dom_.append(node_, t);
  if (!action_label.empty()) {
    action_btn_ = dom_.create("button", "btn", action_label);
    dom_.append(node_, action_btn_);
  }
}

void EmptyState::on_action(EventFn fn) {
  if (action_btn_.valid()) dom_.on(action_btn_, EventKind::kClick, std::move(fn));
}

// ---------- TextInput ----------
TextInput::TextInput(IDom& dom, const std::shared_ptr<Signal<std::string>>& value,
                     const std::string& placeholder)
    : Widget(dom, "input", "") {
  dom_.set_attr(node_, "type", "text");
  if (!placeholder.empty()) dom_.set_attr(node_, "placeholder", placeholder);
  dom_.set_attr(node_, "value", value->get());
  auto v = value;
  dom_.on(node_, EventKind::kInput, [v](const std::string& s) { v->set(s); });
}

// ---------- TextArea ----------
TextArea::TextArea(IDom& dom, const std::shared_ptr<Signal<std::string>>& value,
                   const std::string& placeholder)
    : Widget(dom, "textarea", "") {
  if (!placeholder.empty()) dom_.set_attr(node_, "placeholder", placeholder);
  dom_.set_text(node_, value->get());
  auto v = value;
  dom_.on(node_, EventKind::kInput, [v](const std::string& s) { v->set(s); });
}

// ---------- SearchBox ----------
SearchBox::SearchBox(IDom& dom, ChangeFn on_search, const std::string& placeholder)
    : Widget(dom, "input", "search-box") {
  dom_.set_attr(node_, "type", "search");
  dom_.set_attr(node_, "placeholder", placeholder);
  dom_.on(node_, EventKind::kInput, std::move(on_search));
}

}  // namespace ui
}  // namespace fs

#include "ui_widgets.hpp"

namespace fs {
namespace ui {

// ---------- Button ----------
Button::Button(IDom& dom, const std::string& label, EventFn on_click, bool primary)
    : Widget(dom, "button", primary ? "btn primary" : "btn") {
  dom_.set_text(node_, label);
  dom_.on(node_, EventKind::kClick, std::move(on_click));
}

Button::Button(IDom& dom, const std::shared_ptr<Signal<std::string>>& label,
               EventFn on_click, bool primary)
    : Widget(dom, "button", primary ? "btn primary" : "btn") {
  label_binding_ = bind_text(dom_, node_, label);
  dom_.on(node_, EventKind::kClick, std::move(on_click));
}

void Button::set_disabled(bool disabled) {
  dom_.set_attr(node_, "disabled", disabled ? "1" : "");
}

void Button::set_disabled_sig(const std::shared_ptr<Signal<bool>>& disabled) {
  auto d = disabled;
  IDom* dom = &dom_;
  Node n = node_;
  disabled_binding_ = disabled->subscribe([dom, n, d](const bool&) {
    dom->set_attr(n, "disabled", d->get() ? "1" : "");
  });
  dom_.set_attr(node_, "disabled", disabled->get() ? "1" : "");
}

// ---------- Select ----------
Select::Select(IDom& dom, const std::vector<std::string>& options,
               const std::shared_ptr<Signal<int>>& selected)
    : Widget(dom, "select", "") {
  for (size_t i = 0; i < options.size(); ++i) {
    Node opt = dom_.create("option", "", options[i]);
    dom_.set_attr(opt, "value", std::to_string(i));
    dom_.append(node_, opt);
  }
  auto s = selected;
  dom_.on(node_, EventKind::kChange, [s](const std::string& v) {
    try {
      s->set(std::stoi(v));
    } catch (...) {
    }
  });
  binding_ = selected->subscribe([this](const int& idx) {
    dom_.set_attr(node_, "value", std::to_string(idx));
  });
}

// ---------- Checkbox ----------
Checkbox::Checkbox(IDom& dom, const std::string& label,
                   const std::shared_ptr<Signal<bool>>& checked)
    : Widget(dom, "label", "checkbox") {
  Node box = dom_.create("input", "", "");
  dom_.set_attr(box, "type", "checkbox");
  dom_.append(node_, box);
  Node t = dom_.create("span", "", label);
  dom_.append(node_, t);
  auto c = checked;
  IDom* d = &dom_;
  binding_ = checked->subscribe([d, box, c](const bool&) {
    if (c->get())
      d->set_attr(box, "checked", "1");
    else
      d->set_attr(box, "checked", "");
  });
  dom_.on(box, EventKind::kChange, [c](const std::string& v) { c->set(v == "1" || v == "true" || v == "on"); });
}

// ---------- RadioGroup ----------
RadioGroup::RadioGroup(IDom& dom, const std::vector<std::string>& options,
                       const std::shared_ptr<Signal<int>>& selected)
    : Widget(dom, "div", "radio-group") {
  for (size_t i = 0; i < options.size(); ++i) {
    Node lab = dom_.create("label", "radio", "");
    Node r = dom_.create("input", "", "");
    dom_.set_attr(r, "type", "radio");
    dom_.append(lab, r);
    Node t = dom_.create("span", "", options[i]);
    dom_.append(lab, t);
    int idx = static_cast<int>(i);
    auto s = selected;
    dom_.on(r, EventKind::kChange, [s, idx](const std::string&) { s->set(idx); });
    dom_.append(node_, lab);
  }
}

// ---------- Toggle ----------
Toggle::Toggle(IDom& dom, const std::shared_ptr<Signal<bool>>& on)
    : Widget(dom, "button", "toggle") {
  auto o = on;
  IDom* d = &dom_;
  Node n = node_;
  binding_ = on->subscribe([d, n, o](const bool&) {
    d->set_attr(n, "data-on", o->get() ? "1" : "0");
    d->set_text(n, o->get() ? "开" : "关");
  });
  dom_.set_text(node_, on->get() ? "开" : "关");
  dom_.on(node_, EventKind::kClick, [o](const std::string&) { o->set(!o->get()); });
}

// ---------- Tabs ----------
Tabs::Tabs(IDom& dom, const std::vector<std::string>& titles,
           const std::shared_ptr<Signal<int>>& current,
           const std::vector<PageFn>& pages)
    : Widget(dom, "div", "tabs-wrap"),
      titles_(titles),
      current_(current),
      pages_(pages) {
  bar_ = dom_.create("div", "tabstrip", "");
  dom_.append(node_, bar_);
  body_ = dom_.create("div", "tab-body", "");
  dom_.append(node_, body_);
  auto c = current_;
  Tabs* self = this;
  binding_ = current->subscribe([self, c](const int&) { self->rebuild(); });
  (void)c;
  rebuild();
}

void Tabs::rebuild() {
  dom_.clear(bar_);
  int cur = current_->get();
  for (size_t i = 0; i < titles_.size(); ++i) {
    Node tab = dom_.create("a", i == (size_t)cur ? "tab active" : "tab", titles_[i]);
    int idx = static_cast<int>(i);
    auto c = current_;
    dom_.on(tab, EventKind::kClick, [c, idx](const std::string&) { c->set(idx); });
    dom_.append(bar_, tab);
  }
  dom_.clear(body_);
  if (cur >= 0 && (size_t)cur < pages_.size() && pages_[cur]) {
    pages_[cur](dom_, body_);
  }
}

// ---------- Accordion ----------
Accordion::Accordion(IDom& dom, const std::string& title,
                     const std::shared_ptr<Signal<bool>>& expanded, BodyFn body)
    : Widget(dom, "div", "accordion"),
      title_(title),
      expanded_(expanded),
      body_(std::move(body)) {
  head_ = dom_.create("div", "accordion-head", title_);
  dom_.append(node_, head_);
  body_node_ = dom_.create("div", "accordion-body", "");
  dom_.append(node_, body_node_);
  auto e = expanded_;
  Accordion* self = this;
  dom_.on(head_, EventKind::kClick, [e](const std::string&) { e->set(!e->get()); });
  binding_ = expanded->subscribe([self](const bool&) { self->rebuild(); });
  rebuild();
}

void Accordion::rebuild() {
  dom_.set_visible(body_node_, expanded_->get());
  if (expanded_->get()) {
    dom_.clear(body_node_);
    if (body_) body_(dom_, body_node_);
  }
}

// ---------- ListView ----------
ListView::ListView(IDom& dom, const std::shared_ptr<Signal<int>>& count, int page_size,
                   RowFn row)
    : Widget(dom, "div", "list-view"),
      count_(count),
      page_size_(page_size),
      row_(std::move(row)) {
  body_ = dom_.create("div", "list-body", "");
  dom_.append(node_, body_);
  // v1：全量渲染 count 行（虚拟化：P4 按滚动窗口只渲染可见段）
  ListView* self = this;
  binding_ = count->subscribe([self](const int&) { self->refresh(); });
  refresh();
}

void ListView::refresh() {
  dom_.clear(body_);
  int n = count_->get();
  if (page_size_ > 0 && n > page_size_) n = page_size_;
  for (int i = 0; i < n; ++i) {
    if (row_) row_(dom_, body_, i);
  }
}

// ---------- Modal ----------
Modal::Modal(IDom& dom, const std::shared_ptr<Signal<bool>>& visible,
             const std::string& title, BodyFn body, EventFn on_close)
    : Widget(dom, "div", "modal"),
      visible_(visible),
      title_(title),
      body_(std::move(body)),
      on_close_(std::move(on_close)) {
  Modal* self = this;
  binding_ = visible->subscribe([self](const bool&) { self->rebuild(); });
  rebuild();
}

void Modal::rebuild() {
  dom_.clear(node_);
  if (!visible_->get()) {
    dom_.set_visible(node_, false);
    return;
  }
  dom_.set_visible(node_, true);
  Node box = dom_.create("div", "modal-box", "");
  Node h = dom_.create("div", "modal-title", title_);
  dom_.append(box, h);
  Node b = dom_.create("div", "modal-body", "");
  if (body_) body_(dom_, b);
  dom_.append(box, b);
  Node close = dom_.create("button", "btn", "关闭");
  EventFn oc = on_close_;
  auto v = visible_;
  dom_.on(close, EventKind::kClick, [oc, v](const std::string&) {
    v->set(false);
    if (oc) oc("");
  });
  dom_.append(box, close);
  dom_.append(node_, box);
}

// ---------- Tooltip ----------
void Tooltip::attach(IDom& dom, Node node, const std::string& tip) {
  dom.set_attr(node, "title", tip);  // v1 原生 title；富提示 P4
}

// ---------- Splitter ----------
Splitter::Splitter(IDom& dom, const std::shared_ptr<Signal<int>>& percent)
    : Widget(dom, "div", "splitter") {
  auto p = percent;
  IDom* d = &dom_;
  Node n = node_;
  binding_ = percent->subscribe([d, n, p](const int&) {
    int v = p->get();
    if (v < 10) v = 10;
    if (v > 90) v = 90;
    d->set_attr(n, "data-split", std::to_string(v));
  });
  // v1 固定比例渲染（拖拽 P4）
}

}  // namespace ui
}  // namespace fs