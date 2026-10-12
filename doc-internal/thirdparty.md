# 第三方依赖与出处（3rdparty）

> rules 要求 3rdparty/ 不入 git；`scripts/fetch_3rdparty.bat|.sh` 可重放全部拉取。
> 本文件记录每个依赖的**出处、版本、许可**，供分发时合规与升级参考。

| 目录 | 组件 | 版本 | 许可 | 用途 |
|---|---|---|---|---|
| `filesystem-1.5.14/` | gulrak/filesystem (ghc) | v1.5.14 | MIT | C++14 的 std::filesystem 兼容层 |
| `json/` | nlohmann/json | v3.11.3 | MIT | JSON 读写（`Json` 类型） |
| `dtl-1.20/` | cubicdaiya/dtl | v1.20 | BSD-2 | diff（diffWrite 上下文匹配、diff 展示） |
| `md4c-master/` | mity/md4c | master | MIT | Markdown 解析（UI 高亮/渲染） |
| `httplib/` | yhirose/cpp-httplib | master | MIT | HTTP(S) 客户端（NetClient） |
| `mbedtls-3.6.4/` | Mbed-TLS/mbedtls | v3.6.4 LTS | Apache-2.0 | TLS（httplib 的 mbedTLS 后端） |
| `qjs-regex/` | QuickJS regex engine | 2024-01-13 | MIT | `file.regex` 工具的正则引擎 |

## qjs-regex 明细（addon2：regex 工具）

- 文件：`libregexp.c`、`libregexp.h`、`libregexp-opcode.h`、`libunicode.c`、
  `libunicode.h`、`libunicode-table.h`、`cutils.h`。
- 来源：QuickJS 发行包（Fabrice Bellard，MIT）。本机开发时从 `C:/aaa/WhaleJS/src/`
  的同名文件拷贝（WhaleJS 内嵌同一份 QuickJS 源码）。
- 许可：MIT，Copyright (c) 2017-2018 Fabrice Bellard（见各文件头）。
- 宿主回调（libregexp.h 末尾声明，非 TEST 编译需外部提供）由
  `src/tools/regex_tools.cpp` 实现：
  - `lre_check_stack_overflow` → 恒 false（单线程工具调用，栈充足）
  - `lre_check_timeout` → 恒 0（不设超时；工具级另有整体超时）
  - `lre_realloc` → `std::realloc`
- 上游参考：QuickJS `quickjs.c` 的 `js_regexp_exec` 提供了 capture 数组用法
  （`lre_get_alloc_count` / `lre_get_capture_count`，capture 下标 0..count-1）。
- 升级路径：替换 `scripts/fetch_3rdparty.*` 里的 qjs 版本号并重跑脚本即可。

## 本地 patch

- `dtl-1.20/dtl/Diff.hpp`：`bool trivial;` → `mutable bool trivial;`
  （GCC16 `-Wtemplate-body` 报错，见 notes `<3>`）；由 fetch 脚本自动打。
