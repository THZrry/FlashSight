# 1: 网络走代理

SOLVED

本机无法直连 GitHub（curl exit 6/28），需经本地 v2ray 代理 127.0.0.1:10809
（系统代理已启用：HKCU ProxyServer=127.0.0.1:10809, ProxyEnable=1）。
所有 curl/git clone 加 `-x http://127.0.0.1:10809`。

**经验**：本环境 Bash 实际是 cmd 兼容，无 unix 管道工具（grep/head/tail 不可用），
且每次 Bash 调用是全新进程，`set` 的环境变量不保留，必须写进 .bat 或每条命令带上。
Python 脚本可正常读写文件、打印（用 .py 文件 + 重定向更稳）。

# 2: mbedtls 选型与构建

SOLVED

mbedtls development(4.2.0) 分支不可用：build_info.h include "tf-psa-crypto/build_info.h"，
3rdparty 内无该子模块，且 httplib 对 MBEDTLS_VERSION_MAJOR>=4 走 CPPHTTPLIB_MBEDTLS_V4 分支需 PSA crypto。
改为定稿 **mbedtls 3.6.4 LTS**（release tag 包）。release 包不带 framework 子模块，需：
`git clone --depth 1 https://github.com/Mbed-TLS/mbedtls-framework.git framework`（在 mbedtls-3.6.4/ 下）。
CMake 构建：`-G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
-DCMAKE_C_FLAGS=-Wno-error=format`（GCC16 把 `%I64d` 与 long long 不匹配的 -Wformat 升为 error，仅 2 处 debug 宏）。
链接：`-lws2_32 -lcrypt32 -lbcrypt`。产物：library/libmbedtls.a libmbedx509.a libmbedcrypto.a
（+ 3rdparty/everest/libeverest.a、3rdparty/p256-m/libp256m.a）。

# 3: dtl MinGW patch

SOLVED

dtl v1.20 `Diff::enableTrivial() const`（Diff.hpp:167）给成员 `trivial` 赋值，
GCC16 以 -Wtemplate-body 报错。解法：成员声明 `bool trivial;` → `mutable bool trivial;`（Diff.hpp:66）。
已 patch 到 3rdparty/dtl-1.20/dtl/Diff.hpp，scripts/fetch_3rdparty.bat 里有同样 patch。

# 4: httplib+mbedTLS HTTPS 实测通过

SOLVED

cpp-httplib master（3rdparty/httplib/httplib.h）定义 CPPHTTPLIB_MBEDTLS_SUPPORT 后
与 mbedtls 3.6.4 静态库链接成功，temp/verify 里 verify_https_run.exe 经
`cli.set_proxy("127.0.0.1", 10809)` 请求 raw.githubusercontent.com 返回 200，下载 1403599 字节。
**注意**：httplib 不读环境变量 HTTP_PROXY/HTTPS_PROXY，代理必须应用层显式 set_proxy
（P2/P3 网络工具实现时把代理做成可配置）。

# 5: xmake Windows 配置

SOLVED

本机无 Visual Studio，xmake 默认找不到工具链。配置：
`xmake f -y --toolchain=mingw`（xmake 自动找到 C:/Program Files/mingw64）。
构建：`xmake -r`。空目录 add_files 会产生 warning，用 `os.files()` 展开后只加存在的文件。

# 6: repo 清理

SOLVED

P0 过程中误把 3rdparty_tmp（mbedtls 4.x 解压残留）和若干诊断 scratch 文件提交进 git，
已 `git rm --cached` + 后续干净提交清理。3rdparty/ 与 temp/ 不入 git（rules 要求），
重放方式：`scripts/fetch_3rdparty.bat`（Windows，走代理）。