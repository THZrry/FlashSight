# C++响应式UI框架
> 针对WhaleUI不支持JavaScript的情况，我们可以使用C++编写界面逻辑，然后编译一份wasm版达到双层兼容+高性能

## 架构
- DOM Adapter: WhaleUI提供C风格的完整DOM API，而wasm可以使用emscripten/val.h获取document等包含API的内容，然后封装成统一API
- Container: 数据容器，包装的STL和基础数据类型，用于响应式的事件
- Framework: 使用C++的类构建结构，框架部分负载运行时最小的进行diff和响应式事件处理
- Composers: 预置组件

- user: 业务逻辑，也就是我们的UI
这个完全可以做成动态dom生成（反正C++性能足够高）然后通过constexpr模板将页面提前静态渲染（此处授权使用模板）

## 当前的问题
- Adapter写的及其粗糙，几乎是没有响应式能力的（仅能修改几个显示效果，连事件都没做）
- 没有组件，tab等需要手工实现
- 没有成熟的Frame切换体系

# 现在界面的问题
## 逻辑
- 完全没有事件处理
- 什么都没反应，其余的也提不了

## 显示
- 字小->密集紧凑，内容少->其余地方空旷，没有平衡好
- 按钮效果仅仅是边框变色，过于朴素，做点bg transition会更好
- “探索”“项目”“用量统计”设计中是水平居中的三个小标题性质的按钮，现在做的太小（而且小标题那就只需要字体颜色和decoration变化，不需要background了）
- 原本的设置是“项目”卡片，卡片左部分是项目信息，右部分是可展开的会话列表，目前做成左侧选择右侧标签页，不够合理
- 设置和项目相关按钮太逼仄
- “会话”目前无法进入，但是根据前几轮留下的网页效果不尽人意
- chat历史不像对话工具，这个应该是用户输入纯气泡，输出为正常字体+像文章一样显示，没有气泡、称谓等，而时间、功能按钮使用小字并且hover显示
- chat中的系统消息用浅字号居中显示
- chat中可以有很多流程相关的扩展，普通markdown不一定能显示
- chat是需要折叠功能，但总结不要被折叠且不要边框
- 子代理做的完全不对
- 侧边栏是可新增、关闭的自由标签页，而不是简单的固定tab（因为view file等功能不是常驻且可以用不止一个标签页）
- 终端没做选择器
- 还有很多，不一一列举了

# 构建
> 当前前置构建使用bat，不跨平台且脱离原有工具，需要改掉

首先UI相关代码本身就需要编译2次（wasm+WhaleUI原生），而网页静态资源只需要打包一次，WhaleUI会忽略JavaScript代码，因此静态资源不用过多修改。
启动后程序读取打包进二进制内部的资源文件，对于网页版：开启静态网页+API服务，正常运转；对于WhaleUI桌面版：调用WhaleUI初始化并加载网页，一样使用。

那么问题就在如何高效构建了。`xmake`提供了`set_toolchain`方法，你可以用此方法编译wasm，并且作为dep被依赖构建。伪代码：
```lua
target("wasm-web")
    set_kind("binary")
    set_plat("wasm")
    set_toolchains("emcc")
    add_files("...uicode")

target("FlashSight")
    set_kind("binary")
    add_deps("wasm-web", {link = false})
    add_files("...main")
    add_files("...uicode")
    after_build(function (target)
    	local wasm_target = target:dep("wam-web")
    	local wasm_dir = wasm_target:targetdir()
    	local wasm_file = path.join(wasm_dir, wasm_target:basename() .. ".wasm")
    	[[ Copy and write to executable; Pack resource and clean other files. ]]
	end)
```
