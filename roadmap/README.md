# FlashSight 实现路线（roadmap）

> 顺序：全局 → AppCore → Provider → Agent 引擎 → UI 界面。
> UI 排最后的原因：WhaleUI 未成型，UI 层先只定事件与页面（见 `doc-internal/ui-events.md`），
> 等 WhaleUI 就绪再联调；且 UI 可能独立出 wasm 版，所以单独文件夹 + 单独 target。
> 每份文件用 `- [ ]` 标记，完工一项勾一项；顺序即依赖顺序，不要跳阶段开工。

## 阶段与文件

| 阶段 | 文件 | 内容 | 可并行 |
|---|---|---|---|
| P0 全局 | [00-global.md](00-global.md) | 仓库/xmake/targets/第三方/common/环境记录 | — |
| P1 AppCore | [10-appcore.md](10-appcore.md) | 路径/存储/settings/project/session/plan/usage/lock/terminal-registry/FsApi 骨架 | P0 完成后 |
| P2 Provider | [20-provider.md](20-provider.md) | 四形态+自定义+套餐/密钥/模型表/能力标签 | P1 存储就绪后（用量边写边用） |
| P3 Agent 引擎 | [30-agent.md](30-agent.md) | HistoryBuilder/Loop/Planner/挂起唤醒/ask/过拟合/子代理/内置工具 | P1+P2 完成后 |
| P4 UI 界面 | [40-ui.md](40-ui.md) | fs_ui 独立 target/页面/事件订阅/i18n/主题/联调 | P3 主流程跑通后；WhaleUI 就绪前只做空壳 |

## 里程碑

- M1（P0+P1）：`fs_tests` 能建空会话、读写 full.json、跑通 FsApi 骨架（无模型、无 UI）。
- M2（+P2）：能调通一次真实模型（任一形态），用量记一行 JSONL。
- M3（+P3）：手动模式端到端（send→toolcall→落库→回滚/分叉/遗忘），自动/挂起/唤醒单测全过。
- M4（+P4）：UI 联调，会话页四区 + 设置 + 用量页可用（WhaleUI 就绪为前提）。

## 开工前 TODO（阻塞 M1）

- [ ] 验证 httplib + mbedTLS 在 Win/Linux 编译与 HTTPS 可用性，不可用回退 libcurl（见 build.md §3）。
- [ ] ghsl.filesystem / nlohmann/json / dtl / md4c 引入 3rdparty 并在 xmake 里接好。
- [ ] `temp/environment.md` 记录 python/C++ 编译器版本。
