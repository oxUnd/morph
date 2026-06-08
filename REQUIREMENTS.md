# 多题材 Agent 需求文档

> **文档版本**: v0.5
> **状态**: Updated — 同步代码实际行为（v0.3.0），新增 MCP / 记忆 / 子代理 / 可插拔 Guardrail / FastCGI / BPE Tokenizer

## 0. 术语与缩写

| 术语 | 定义 |
|------|------|
| Agent | 本产品运行实体，封装 ReAct 循环、工具与会话 |
| ReAct | Reasoning + Acting，LLM 输出 Thought/Action，运行时回填 Observation 的循环 |
| Tool | Agent 可调用的能力单元，包含**内置工具**与**Ext**两类 |
| Ext | 用户/社区编写的可热插拔扩展，以 `.so` 或可执行文件形态存在，运行在沙箱中 |
| Session | 一段连续对话的上下文集合，持久化在 SQLite |
| Context Window | 单次 LLM 调用送入的 token 总量 |
| MVP | Minimum Viable Product，M1 里程碑交付物 |
| MCP | Model Context Protocol，标准化的远程工具/资源/提示词发现协议 |
| Sub-agent | 可配置的子代理，拥有独立系统提示、模型和工具限制 |
| Memory | 长期记忆系统，跨会话保留事实、经历和程序性知识 |

---

## 1. 产品概述

### 1.1 定位
面向内容创作者的 **CLI 多题材 AI Agent**，纯 C 实现，以终端对话驱动文字/图片/视频的理解与生成，帮助创作者完成从选题到成品的全流程。

### 1.2 暂定名
**morph**

### 1.3 差异化价值
1. **一站式跨模态**：文字、图片、视频统一入口，ReAct 自动编排
2. **终端原生**：零 GUI 依赖，可嵌入 shell 管道与 CI 流水线
3. **纯 C + 零运行时**：编译即用，内存占用低，启动 < 50ms
4. **可扩展**：Ext 沙箱机制允许任意语言编写扩展，主进程不被污染
5. **本地优先**：会话与产物全部本地持久化，离线可回看
6. **MCP 集成**：通过 Model Context Protocol 接入远程工具生态
7. **子代理编排**：可配置专用子代理处理特定领域任务
8. **长期记忆**：跨会话记忆事实、经历和行为模式

### 1.4 非目标（Non-Goals）
- 不提供原生 GUI（可选 FastCGI 前端用于 Web API 集成，见 §4.13）
- 不在端侧运行大模型推理（始终通过 API 调用）
- 不替代专业剪辑工具（只做生成，不做后期工程）
- MVP 不支持 Windows 原生（WSL 可用）

---

## 2. 目标用户与场景

| 角色 | 核心诉求 | 典型场景 |
|------|---------|---------|
| 短视频创作者 | 快速产出脚本+素材 | 写脚本 → 生成配图 → mpv 预览 |
| 图文自媒体 | 批量图文 | 选题 → 文案 → 配图 → 导出 |
| 设计师 | 视觉原型探索 | 描述 → 参考图 → 迭代 |
| 小说/编剧 | 故事线+分镜 | 大纲 → 角色卡 → 分镜图 |
| 开发者 | 自动化内容流水线 | CLI 管道 → 批量生成 |
| Ext 开发者 | 扩展 Agent 能力 | manifest + 编译 → 注册 |
| Web 集成者 | 通过 HTTP API 接入 | FastCGI + nginx 部署 |

---

## 3. 成功度量（KPI）

> 每个 P0 功能必须可被以下指标度量；未达标视为该里程碑未交付。

| 维度 | 指标 | 目标 |
|------|------|------|
| 性能 | 文字首 token 延迟（P95） | < 1.0s |
| 性能 | 图片生成端到端（P95） | < 30s |
| 性能 | 冷启动到 prompt 就绪 | < 200ms |
| 资源 | 静态可执行文件大小（动态链接） | < 8MB |
| 资源 | 空闲常驻内存 | < 15MB |
| 资源 | 单次对话峰值内存 | < 80MB |
| 质量 | 单元测试覆盖率（核心模块） | ≥ 70% |
| 质量 | Valgrind 内存泄漏 | 0 definitely lost |
| 安全 | Ext 沙箱逃逸用例（见 §10） | 100% 阻断 |
| 兼容 | macOS 14+ / Ubuntu 22.04+ / Arch Linux 冒烟测试 | 100% |

---

## 4. 功能需求

### 4.1 优先级定义

- **P0**：MVP（M1）必须交付
- **P1**：V0.2~V0.4 交付
- **P2**：V1.0 前交付
- **P3**：愿景项，后续评估

### 4.2 对话系统

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 多轮对话 | 上下文记忆，支持追问与修改 | P0 |
| 会话管理 | 创建 / 切换 / 重命名 / 删除 | P0 |
| 历史回溯 | 列表 + 按 ID 加载 | P0 |
| 长上下文 | 至少 128K tokens（取决于所选模型） | P1 |
| 多语言 | 中文为主，兼容英文 | P2 |
| 多模型并行 | 同时与多个模型对比 | P3 |

### 4.3 上下文压缩

| 功能 | 描述 | 优先级 |
|------|------|--------|
| Token 计数 | 调用前精确估算，支持 BPE（CL100K/O200K）+ Unicode-Aware 估算 | P0 |
| 滑动窗口 | 保留最近 N 轮 + 系统提示 | P0 |
| 自动摘要 | 超阈值时，对早期历史摘要 | P0 |
| `/context` 命令 | 查看 token 用量与压缩状态 | P0 |
| `/compress` 命令 | 手动触发 | P0 |
| 系统提示压缩 | 按需裁剪工具描述长度 | P1 |
| 关键信息提取 | 文件路径、生成结果、用户偏好强制保留 | P1 |
| ReAct 轨迹压缩 | 已完成循环只保留 Final | P1 |
| 多媒体引用压缩 | 上下文只保留路径+摘要 | P1 |
| 递归摘要 | 摘要再摘要 | P2 |

### 4.4 文字生成

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 文案创作 | 主题/关键词 → 文章/脚本/标题 | P0 |
| 文本改写 | 润色、缩扩写、风格转换 | P0 |
| 流式输出 | SSE 逐 token | P0 |
| 多格式 | Markdown / 纯文本 / 大纲 | P1 |
| 模板系统 | 内置脚本/推文/小红书等模板 | P1 |

### 4.5 图片生成与理解

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 文生图 | 文字 → 图片 | P0 |
| 图片理解 | 上传 → 描述/分析/问答 | P0 |
| 终端预览 | sixel / kitty / iterm2 | P0 |
| 图片缩放 | stb_image_resize2 | P0 |
| 图片格式转换 | stb_image + stb_image_write | P0 |
| 图片信息查询 | stb_image（尺寸/格式等） | P0 |
| 图片标注 | 在图片上添加文字/图形标注 | P0 |
| 图片编辑 | 局部修改、风格迁移 | P1 |
| 一致性生成 | 同一角色多场景 | P1 |
| 批量生成 | 一次多变体 | P2 |

### 4.6 视频生成与理解

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 文生视频 | 文字 → 短视频 | P0 |
| 图生视频 | 图片 → 短视频 | P0 |
| 异步轮询 | 长任务后台等待 | P0 |
| 视频理解 | 上传 → 摘要/问答（抽帧或 API） | P0 |
| mpv 播放 | fork+exec | P0 |
| 时长 3~30s | — | P1 |
| 视频编辑 | 拼接 / 字幕 / 转场 | P2 |
| 风格化 | 动漫 / 写实 / 胶片 | P2 |

### 4.7 ReAct 推理协同

Agent 基于 ReAct 模式自主规划跨模态任务，用户无需手动指定模态路由：

| 场景 | ReAct 轨迹示例 |
|------|----------------|
| 文图联动 | Thought: 生成文案 → Action: text_gen → Obs: 文案 → Thought: 配图 → Action: img_gen → Final: 文案+配图 |
| 图视联动 | Thought: 图片变视频 → Action: vid_gen(image) → Final: 视频文件 |
| 全链路 | Thought: 选题 → Action: text_gen → Thought: 配图 → Action: img_gen → Thought: 动态化 → Action: vid_gen → Final: 完整作品 |
| 子代理委派 | Thought: 需要专业翻译 → Action: agent_delegate(translate-agent, task) → Obs: 翻译结果 → Final |

### 4.8 Ext 沙箱系统

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 自动发现 | 扫描目录，解析 manifest.toml | P0 |
| 注册到 Tool Registry | Ext 即 Tool | P0 |
| 沙箱执行（seccomp + rlimit） | Linux | P0 |
| Guardrail Ext | Ext 可作为 Guardrail 规则插件（.so） | P0 |
| 沙箱执行（sandbox-exec） | macOS | P2（未实现 `sandbox_enter_darwin`） |
| Ext 启停 | enable / disable | P1 |
| Ext 安装/卸载 | install / remove（本地路径） | P1 |
| 远程仓库拉取 | git clone | P2 |
| Namespace 隔离（PID/NET） | 强隔离 | P2 |

> **MVP 范围裁剪**：M1 的 Ext 子系统只交付**最小闭环**——发现 + 注册 + 基础 seccomp 沙箱。`enable/disable/remove`、远程拉取、macOS 沙箱推迟到 V0.4。

### 4.9 长期记忆系统

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 事实记忆（Facts） | 提取用户偏好、项目信息等结构化事实 | P0 |
| 经历记忆（Episodes） | 记录任务执行过程、工具使用、结果 | P0 |
| 程序性记忆（Procedures） | 学习重复性行为模式为规则 | P0 |
| LLM 驱动提取 | 使用 LLM 进行结构化记忆提取 | P0 |
| 启发式回退 | LLM 不可用时使用锚点启发式提取 | P0 |
| 异步整合 | 后台线程异步整合记忆，不阻塞主流程 | P0 |
| 记忆上下文注入 | 构建记忆上下文注入 ReAct 推理 | P0 |
| `/memory` 命令 | 查看/清除记忆 | P0 |
| 跨会话持久化 | SQLite 持久化，会话级作用域 | P0 |

### 4.10 MCP（Model Context Protocol）

| 功能 | 描述 | 优先级 |
|------|------|--------|
| stdio 传输 | fork+exec 子进程，stdin/stdout JSON-RPC | P0 |
| Streamable HTTP 传输 | libcurl HTTP 连接 | P0 |
| 工具发现与注册 | tools/list → 自动注册为 morph 工具 | P0 |
| 资源发现与读取 | resources/list + resources/read | P0 |
| 提示词发现与获取 | prompts/list + prompts/get | P0 |
| 多服务器管理 | 最多 32 个 MCP 服务器 | P0 |
| 自动连接 | 启动时自动连接 + 超时控制 | P0 |
| `/mcp` 命令 | list/tools/resources/prompts/connect/disconnect | P0 |
| 延迟连接 | 按需连接（lazy connect） | P0 |

### 4.11 子代理系统

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 可配置子代理 | 独立系统提示、模型、工具限制 | P0 |
| 同步调用 | agent_sync：阻塞等待结果 | P0 |
| 异步委派 | agent_delegate：后台执行 + agent_status 查询 | P0 |
| 并行扇出 | agent_fanout：多任务并行 + 合并策略 | P0 |
| 上下文策略 | full / summary / task_only | P0 |
| 合并策略 | synthesize / concat / raw | P0 |
| 输出 Schema | 约束子代理输出格式 | P0 |
| 递归深度限制 | 最大嵌套深度 2 层 | P0 |
| 追踪日志 | 子代理执行轨迹记录 | P0 |

### 4.12 可插拔 Guardrail 引擎

| 功能 | 描述 | 优先级 |
|------|------|--------|
| C 函数规则 | 内置 C 函数实现验证逻辑 | P0 |
| LLM 评估规则 | 使用 LLM 进行主观质量评估 | P0 |
| Ext 插件规则 | .so 动态库作为验证插件 | P0 |
| 三个 Hook 点 | input / tool_output / output | P0 |
| 配置驱动 | 通过 config.toml 注册/启停规则 | P0 |
| 内置规则 | 自动注册的默认验证规则 | P0 |
| 失败重试 | Guardrail 失败回灌原因，LLM 重试 | P0 |

### 4.13 FastCGI Web 前端

| 功能 | 描述 | 优先级 |
|------|------|--------|
| RESTful API | 会话 CRUD + 对话 turns + canvas | P0 |
| SSE 流式输出 | 实时推送 thought/tool_call/final 等事件 | P0 |
| SQLite WAL 共享 | CLI 与 FastCGI 共享同一数据库 | P0 |
| Bearer 认证 | API Key 认证 | P0 |
| X-Remote-User 认证 | 反向代理信任头认证 | P0 |
| 可选构建 | `BUILD_FASTCGI=ON` CMake 选项 | P0 |
| Action 机制 | Web → Agent 命令注入（approve/reject/cancel） | P0 |

> **定位**：FastCGI 前端是可选的 HTTP API 层，用于将 morph agent 集成到 Web 系统。不提供原生 GUI，Web UI 由外部前端实现。

---

## 5. CLI 交互设计

### 5.1 启动与对话示例

```text
$ morph

morph v0.3  |  /help 查看命令

[abc1] $ 帮我写一个赛博朋克短视频脚本，并配图和视频
⠋ Thinking → 赛博朋克短视频脚本...
-> text_gen → prompt="赛博朋克短视频脚本"
OK Done (3s)
⠋ Thinking → 根据第二幕生成配图...
-> img_gen → prompt="赛博朋克街道夜景，霓虹灯", style="realistic"
OK Image generated (8s)
⠋ Thinking → 将配图扩展为 5s 视频...
-> vid_gen → image="img_20260512_01.png", duration=5
OK Video generated (25s)
[Guardrail] No tools called but output expected. Try using available tools.

(最终结果经 Markdown→ANSI 渲染输出，图片在终端内联预览，视频用 mpv 播放)

[abc1] $ /context
context: 4321 / 128000 tokens (3.4%) | messages: 5
[abc1] $ /trace
--- ReAct trace (赛博朋克短视频)
  1. [Thought] 用户需要脚本+配图+视频。先写脚本。
  2. [Action] text_gen(prompt="赛博朋克短视频脚本") (tool: text_gen)
  3. [Observation] 霓虹灯在雨幕中闪烁……
  4. [Thought] 根据第二幕生成配图。
  5. [Action] img_gen(prompt="赛博朋克街道夜景") (tool: img_gen)
  6. [Observation] image generated: ~/.morph/output/img_20260512_01.png
  7. [Thought] 将配图扩展为 5s 视频。
  8. [Action] vid_gen(image="img_20260512_01.png") (tool: vid_gen)
  9. [Observation] video generated: ~/.morph/output/vid_20260512_01.mp4
  10. [Guardrail] No tools called but output expected
  11. [Final] 完成，图片已在终端预览，视频用 mpv 播放。
state: DONE, steps: 11
[abc1] $ /save
--- saving session to 赛博朋克短视频_1715817600.md
saved 5 messages
```

> **说明**：ReAct 过程中不再逐行打印 Thought/Action/Observation，而是通过 Spinner 动画在单行内指示当前状态（见 §6.11）。仅在 `/trace` 命令中展示完整步骤列表。Guardrail 验证失败时会直接打印 `[Guardrail]` 标签行。当 HITL 启用时，工具执行前会提示用户确认（`[y/n/a]`）。

### 5.2 输入方式

| 类型 | 方式 | 示例 |
|------|------|------|
| 文字 | 直接输入 | `写一段日落文案` |
| 图片 | 文件路径（自动识别后缀） | `分析 ./photo.jpg` |
| 视频 | 文件路径 | `总结 ./clip.mp4` |
| 混合 | 文本 + 路径 | `把 ./img.png 变成视频，写实` |
| 命令 | `/` 前缀 | `/help` `/new` … |

### 5.3 输出渲染

| 输出 | 渲染 | 备注 |
|------|------|------|
| ReAct 过程 | Spinner 动画（单行，4 样式可选：dots/arrow/pulse/braille） | 状态前缀：`>>` 思考 / `->` 执行 / `OK` 完成 / `ERR` 错误 |
| Thought 流式预览 | Spinner submessage 滚动显示最近 token | 超长自动滚动，最多 80 列可见 |
| 耗时 | 阶段完成后显示 `(Ns)` 或 `(Nm Ns)` | 紧跟状态前缀 |
| Guardrail | 直接打印 `[Guardrail]` 标签行 | 粗体青色 |
| 文字（Final） | 自实现 Markdown→ANSI（基于 md4c）+ 媒体内联回调 + 代码高亮 + LaTeX Unicode | 颜色 / 粗体 / 代码块 / 语法高亮 / 数学公式；Markdown 中的 `![image](path)` / `![video](path)` 自动触发渲染 |
| 图片 | 终端协议优先级：**kitty > iterm2 > sixel > 文件路径回退** | 自动探测 |
| 视频 | fork+exec mpv | 失败回退到打印路径 |
| 链接 | OSC 8 超链接 | 不支持时降级为纯 URL |

### 5.4 完整命令表

> 每个命令需在 `--help` 与 README 中保持文案一致（测试用例覆盖）
> 主命令与短别名共享同一 handler，帮助中合并显示。

| 命令 | 别名 | 功能 | 示例 |
|------|------|------|------|
| `/quit` | `/q` | 退出 | `/q` |
| `/help [cmd]` | `/h` | 帮助 | `/h ext` |
| `/new [name]` | `/n` | 新建会话 | `/n 项目A` |
| `/switch <name\|id\|display_id>` | `/s` | 切换会话（支持 `^N` 索引） | `/s abc1` |
| `/list [filter] [--all]` | `/ls` | 会话列表（含 display_id，支持过滤） | `/ls --all` |
| `/rename <new>` | `/rn` | 重命名当前会话 | `/rn 项目B` |
| `/delete <name\|id>` | `/del` | 删除会话 | `/del 3` |
| `/history [n\|--all]` | `/hi` | 当前会话最近 n 条 | `/hi --all` |
| `/model [name]` | `/m` | 查看/切换当前模型 | `/m gpt-4o` |
| `/trace [--from-db]` | `/t` | 当前轮次 ReAct 轨迹 | `/t --from-db` |
| `/context` | `/ctx` | token 用量与上下文信息 | — |
| `/compress` | `/cp` | 手动触发压缩 | — |
| `/save [format]` | — | 导出会话（md/json/txt） | `/save md` |
| `/export <fmt>` | — | `/save` 别名 | — |
| `/config` | `/cfg` | 查看当前配置 | — |
| `/image <path>` | `/img` | 注入图片 | `/img ./photo.jpg` |
| `/video <path>` | `/vid` | 注入视频 | `/vid ./clip.mp4` |
| `/ext list` | `/x list` | 已注册工具列表 | — |
| `/ext info <name>` | — | 工具详情 | `/ext info text_gen` |
| `/ext install <path>` | — | 本地路径安装（stub） | — |
| `/ext enable <name>` | — | 启用（stub） | — |
| `/ext disable <name>` | — | 禁用（stub） | — |
| `/ext remove <name>` | — | 卸载（stub） | — |
| `/skill list` | `/sk list` | 已发现 Skill 列表 | — |
| `/skill info <name>` | — | Skill 详情 | `/skill info code-review` |
| `/skill activate <name>` | — | 激活 Skill | `/skill activate code-review` |
| `/skill deactivate <name>` | — | 停用 Skill | `/skill deactivate code-review` |
| `/render <path>` | `/r` | 渲染文件（图片/视频/Markdown） | `/r output.png` |
| `/mcp list` | — | 已配置 MCP 服务器列表 | — |
| `/mcp tools <server>` | — | 服务器提供的工具 | — |
| `/mcp resources <server>` | — | 服务器提供的资源 | — |
| `/mcp prompts <server>` | — | 服务器提供的提示词 | — |
| `/mcp connect <server>` | — | 连接 MCP 服务器 | — |
| `/mcp disconnect <server>` | — | 断开 MCP 服务器 | — |
| `/memory show` | `/mem show` | 查看当前会话记忆 | — |
| `/memory clear [scope]` | `/mem clear` | 清除记忆（all/facts/episodes/procedures） | `/mem clear facts` |
| `/clear` | `/cl` | 清屏 | — |

> **提示符格式**：`[display_id] $ `，其中 `display_id` 为 4 字符短标识，在创建会话时自动生成，便于快速引用。
> **stub 标注**：`/ext install/enable/disable/remove` 当前为 stub 实现，打印 "not yet implemented"。

---

## 6. 技术架构

### 6.1 整体分层

```
┌─────────────────────────────────────────────┐
│  CLI 交互层（readline + ANSI + 终端图像）    │
│  FastCGI 前端（可选，RESTful + SSE）         │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  ReAct Agent 核心                           │
│  ┌──────────────────────────────────────┐   │
│  │ LLM 推理（Thought/Action/Obs/Final）│   │
│  ├──────────────────────────────────────┤   │
│  │ Tool Registry（内置 + Ext + MCP）   │   │
│  ├──────────────────────────────────────┤   │
│  │ Sub-agent 子代理编排                 │   │
│  ├──────────────────────────────────────┤   │
│  │ Guardrail 可插拔规则引擎            │   │
│  ├──────────────────────────────────────┤   │
│  │ Context Manager（token/压缩/摘要）  │   │
│  ├──────────────────────────────────────┤   │
│  │ Memory（事实/经历/程序性记忆）       │   │
│  ├──────────────────────────────────────┤   │
│  │ Plan（多步计划管理）                 │   │
│  ├──────────────────────────────────────┤   │
│  │ Ext Sandbox（seccomp/rlimit/ns）  │   │
│  └──────────────────────────────────────┘   │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  模型接入层                                 │
│  LLM API   |  图像 API  |  视频 API         │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  存储 / 文件 / 日志                          │
│  SQLite | ~/.morph/output | log file        │
└─────────────────────────────────────────────┘
```

### 6.2 ReAct 实现规范

#### 6.2.1 单步状态机

```
INIT → THINKING → ACTING → OBSERVING → THINKING → ... → GUARDRAIL → FINAL → DONE
                    ↘ HITL_DENY → OBSERVING(denied) → THINKING
                    ↘ TOOL_FAIL → THINKING（重试，带错误上下文）
                    ↘ MAX_ITER  → ABORT（返回部分结果）
                    ↘ GUARDRAIL_FAIL → THINKING（Guardrail 验证失败，回灌原因重试）
```

#### 6.2.2 终止条件（必须全部实现）

1. LLM 返回无工具调用的文本响应 → 进入 Guardrail 验证
2. 步数达到 `max_iterations`（默认 10，可配置） → 返回最后一次 Observation 并标记 `aborted`
3. 单步耗时超过 `step_timeout_seconds`（默认 330s） → 中断该工具调用，生成失败 Observation 回灌
4. 用户按 `Ctrl-C` → 优雅取消，保存已完成步骤到会话
5. 当 `guardrail_enabled` 时，LLM 输出最终回答后进入 `GUARDRAIL` 状态，由可插拔规则引擎验证结果质量；若 Guardrail 验证失败则回到 `THINKING` 重试（最多 `guardrail_max_retries` 次，默认 1）

#### 6.2.3 工具失败处理

- 工具返回非零 → 构造形如 `Observation: tool error: <msg>` 的观察项回灌给 LLM
- 同一工具连续失败 ≥ 3 次 → 强制 `Final` 并报错给用户

#### 6.2.4 Prompt 模板（完整）

系统提示定义在 `src/agent/system_prompt.h`，以 `MORPH_SYSTEM_PROMPT` 宏实现，包含两个格式化占位符（`%s` 当前时间、`%d` 最大迭代次数）。

提示核心定位为通用 AI Agent，包含 6 条通用原则（Intent Understanding、Planning、Tool Selection、Result Verification、Error Recovery、Collaboration）以及通用工作流、技能提示和规则。

创作能力通过 `skills/creation/SKILL.md` 技能提供，包含 12 条创意原则、创作工具使用指导、创作工作流、质量检查清单和错误恢复策略。LLM 在识别到用户有创作/视觉生产需求时，自动调用 `activate_skill("creation")` 加载完整创作指令。

运行时 `build_system_prompt()` 依次拼接：
1. `MORPH_SYSTEM_PROMPT` 基础模板（通用 Agent）
2. `system_prompt` 自定义扩展（来自配置 `system_prompt_file` + `system_prompt_dir`）
3. 记忆上下文（`memory_context`，来自 Memory 子系统）
4. 已发现 Skill 目录
5. 已激活 Skill 的完整指令内容（含创作技能）

```
You are Morph, an autonomous AI agent.
...
Maximum %d tool-calling iterations.
```

> **注意**：morph 已从文本解析（`Thought: ... Action: ... Final: ...`）迁移到 OpenAI Function Calling。LLM 的结构化工具调用通过 `tool_calls` 字段传递，无需文本格式约束。

### 6.3 上下文压缩流程（层级 fallback）

```
LLM 调用前:
  1. tokenize(messages) → total_tokens
  2. if total_tokens < threshold → 直接发送，结束
  3. compress_detect_react_cycles(messages)  ← 标记循环模式消息
  4. compress_react_trace(messages)          ← 移除已标记的压缩消息
  5. compress_summarize(早期对话 → 单条 summary)
  6. if 仍超阈值: compress_sliding_window(keep=N rounds)
  7. extract_key_info(messages) → preserve list
  8. inject(preserve list + 压缩后 messages) → LLM
```

> 关键修订：步骤之间是「层级 fallback」而非并列。`compress_detect_react_cycles` 先标记循环，`compress_react_trace` 再移除已标记消息。`compress_summarize` 使用注入的 `summarize_fn` 回调（react.c 中使用 LLM 实现），而非直接依赖 `struct model *`。`compress_recursive` 和 `compress_system_prompt` 尚未实现（P1/P2）。

### 6.4 技术选型

| 组件 | 选型 | 必需 | 备注 |
|------|------|------|------|
| 构建系统 | CMake ≥ 3.20 | ✓ | 跨平台 + FetchContent |
| 编译器 | GCC ≥ 10 / Clang ≥ 14，C11 | ✓ | — |
| CLI 输入 | readline（GNU）/ editline（BSD） | ✓ | macOS 默认 editline |
| HTTP / SSE | libcurl（系统库） | ✓ | 工业级 |
| TLS | libcurl 内置（OpenSSL 或 GnuTLS） | ✓ | — |
| JSON | cJSON | ✓ | 单文件嵌入（vendor/） |
| TOML | toml（tomlc99 fork） | ✓ | 单文件嵌入（vendor/），需 `-include vendor_toml_compat.h` |
| Markdown | md4c v0.5.3 | ✓ | CMake FetchContent 拉取，非 vendor/ 嵌入 |
| 代码高亮 | 自实现 ANSI 语法高亮 | ✓ | `src/render/highlight.c` |
| LaTeX 渲染 | 自实现 LaTeX→Unicode | ✓ | `src/render/latex_unicode.c` |
| 图片解码/编码/缩放 | stb_image / stb_image_write / stb_image_resize2 | ✓ | 单头文件（vendor/） |
| 终端图像 | 自实现 kitty/iterm2/sixel 协议 | ✓ | 缺失时回退路径 |
| Token 计数 | BPE（CL100K/O200K）+ Unicode-Aware 估算 | ✓ | `src/util/bpe.c` + `src/agent/tokenizer.c` |
| UTF-8 | sheredom/utf8.h + 项目扩展 | ✓ | vendored header-only + `src/util/utf8.c` |
| 异步 / 子进程 | fork+exec + waitpid | ✓ | 无需 libuv |
| 多线程 | pthreads | ✓ | — |
| 持久化 | sqlite3（系统库） | ✓ | 单文件 DB |
| 沙箱（Linux） | libseccomp + setrlimit + landlock（可选） | ✓ | 主平台 |
| 沙箱（macOS） | sandbox-exec（系统命令） | ✓ | 子进程方式（P2，未实现） |
| 视频播放 | fork+exec `mpv`（系统二进制） | ✓ | 用户指定 |
| 抽帧（可选） | ffmpeg（系统二进制） | 可选 | 视频理解本地 fallback |
| 日志 | 自实现（stderr + rotating file） | ✓ | 见 §9 |
| MCP 协议 | 自实现 JSON-RPC（stdio + HTTP） | ✓ | 协议版本 2025-06-18 |
| FastCGI | libfcgi（可选构建） | 可选 | `BUILD_FASTCGI=ON` |

### 6.5 模型/API 接入策略

| 能力 | 推荐 API | 备注 |
|------|----------|------|
| 文字对话 | OpenAI GPT-4o / Claude 3.5 / DeepSeek / Volcengine | 多模型可切 |
| 文生图 | DALL-E 3 / Volcengine Seedream / Stable Diffusion API | — |
| 图片理解 | GPT-4o Vision / Claude Vision | — |
| 文生视频 | Volcengine / 可灵 / 即梦 / Runway | 国内优先 |
| 图生视频 | Volcengine / 可灵 / 即梦 / Pika | 国内优先 |
| 视频理解 | GPT-4o / Gemini + 本地 ffmpeg 抽帧 | 双路 |

### 6.6 数据持久化（SQLite Schema）

```sql
CREATE TABLE sessions (
	id          INTEGER PRIMARY KEY AUTOINCREMENT,
	name        TEXT UNIQUE NOT NULL,
	display_id  TEXT UNIQUE,
	model       TEXT,
	created_at  INTEGER NOT NULL,
	updated_at  INTEGER NOT NULL,
	token_used  INTEGER DEFAULT 0
);

CREATE TABLE messages (
	id          INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id  INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
	role        TEXT NOT NULL,
	content     TEXT NOT NULL,
	token_count INTEGER NOT NULL,
	compressed  INTEGER DEFAULT 0,
	created_at  INTEGER NOT NULL
);

CREATE TABLE message_attachments (
	id          INTEGER PRIMARY KEY AUTOINCREMENT,
	message_id  INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
	kind        TEXT NOT NULL,
	path        TEXT NOT NULL,
	sha256      TEXT
);

CREATE TABLE react_traces (
	id          INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id  INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
	round_no    INTEGER NOT NULL,
	steps_json  TEXT NOT NULL,
	aborted     INTEGER DEFAULT 0,
	created_at  INTEGER NOT NULL
);

CREATE TABLE exts (
	id           INTEGER PRIMARY KEY AUTOINCREMENT,
	name         TEXT UNIQUE NOT NULL,
	version      TEXT,
	path         TEXT NOT NULL,
	type         TEXT NOT NULL,
	permissions  INTEGER,
	enabled      INTEGER DEFAULT 1,
	installed_at INTEGER NOT NULL
);

CREATE TABLE outputs (
	id         INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,
	kind       TEXT NOT NULL,
	path       TEXT NOT NULL,
	prompt     TEXT,
	model      TEXT,
	created_at INTEGER NOT NULL
);

CREATE TABLE memory_profiles (
	session_id  INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,
	profile_text TEXT,
	updated_at  INTEGER NOT NULL
);

CREATE TABLE memory_facts (
	id          INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id  INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
	key_name    TEXT NOT NULL,
	value_text  TEXT NOT NULL,
	source_text TEXT,
	confidence  REAL DEFAULT 1.0,
	category    TEXT,
	importance  REAL DEFAULT 0.5,
	is_current  INTEGER DEFAULT 1,
	valid_from  INTEGER,
	valid_to    INTEGER,
	superseded_by INTEGER,
	created_at  INTEGER NOT NULL,
	updated_at  INTEGER NOT NULL
);

CREATE TABLE memory_episodes (
	id           INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id   INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
	task_type    TEXT,
	summary_text TEXT NOT NULL,
	outcome_text TEXT,
	success      INTEGER,
	entities     TEXT,
	key_decisions TEXT,
	artifacts    TEXT,
	tools_used   TEXT,
	importance   REAL DEFAULT 0.5,
	created_at   INTEGER NOT NULL
);

CREATE TABLE memory_procedures (
	id           INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id   INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
	rule_text    TEXT NOT NULL,
	trigger_text TEXT,
	evidence_count INTEGER DEFAULT 1,
	updated_at   INTEGER NOT NULL,
	UNIQUE(session_id, rule_text)
);

CREATE INDEX idx_messages_session  ON messages(session_id, created_at);
CREATE INDEX idx_traces_session    ON react_traces(session_id, round_no);
CREATE INDEX idx_outputs_session   ON outputs(session_id, created_at);
CREATE INDEX idx_memory_facts_session_key ON memory_facts(session_id, key_name);
CREATE INDEX idx_memory_episodes_session  ON memory_episodes(session_id, created_at);
CREATE INDEX idx_memory_procedures_session ON memory_procedures(session_id);
```

### 6.7 配置文件 Schema

`~/.morph/config.toml`（权限 0600）：

```toml
[general]
default_session = "default"
output_dir = "~/.morph/output"
log_level = "info"
log_file = "~/.morph/log/agent.log"

[model.text]
provider = "openai"
model = "gpt-4o"
api_base = "https://api.openai.com/v1"
api_key_env = "OPENAI_API_KEY"
context_limit = 128000
max_tokens = 4096
timeout_seconds = 300

[model.image]
provider = "openai"
model = "dall-e-3"
api_key_env = "OPENAI_API_KEY"

[model.video]
provider = "volcengine"
model = "doubao-seedream"
api_key_env = "VOLCENGINE_API_KEY"
poll_interval_seconds = 5
poll_timeout_seconds = 600

[react]
max_iterations = 10
step_timeout_seconds = 330
tool_max_retries = 3
guardrail_enabled = true
guardrail_max_retries = 1
guardrail_max_empty_rounds = 2
disabled_tools = []

# Guardrail 规则配置
guardrail_disabled_rules = []
guardrail_llm_model = ""

# Guardrail LLM 评估规则
# [[react.guardrail_llm_rules]]
# name = "quality-check"
# hook = "output"
# description = "Check output quality"
# action_text = "Improve the output quality"

# Guardrail Ext 插件规则
# [[react.guardrail_ext_rules]]
# name = "pii-check"
# hook = "output"
# ext_type = "so"
# ext_entry = "pii_check.so"
# action_text = "Remove PII from output"

# bash_exec 配置
bash_exec_enabled = true
bash_exec_default_timeout = 30
bash_exec_allowed_commands = []
bash_exec_allowed_cwds = []

# Human-in-the-Loop (HITL)
hitl_enabled = false
# hitl_tools = ["bash_exec", "img_gen", "vid_gen"]
hitl_auto_approve_readonly = true

[context]
summarize_threshold_ratio = 0.8
compress_target_ratio = 0.5
keep_recent_rounds = 6

[memory]
enabled = true
hot_path_enabled = true
cold_path_enabled = true
llm_extract_enabled = true
max_facts = 100
max_episodes = 50
max_procedures = 30
max_context_chars = 4000

[render]
prefer_image_protocol = "auto"
mpv_args = "--really-quiet"

[skill]
dir = "~/.morph/skills"

[prompt]
system_prompt_file = ""
system_prompt_dir = ""

[ext]
dir = "~/.morph/exts"
default_max_memory_mb = 128
default_max_cpu_seconds = 30

# MCP 服务器配置
# [[mcp.servers]]
# name = "my-server"
# transport = "stdio"
# command = "npx"
# args = ["-y", "@my/mcp-server"]
# env = { "API_KEY" = "my-key" }
# auto_connect = true
# connect_timeout = 10

# [[mcp.servers]]
# name = "remote-server"
# transport = "http"
# url = "https://mcp.example.com"
# auth_token_env = "MCP_AUTH_TOKEN"
# auto_connect = true
# connect_timeout = 15

# 子代理配置
# [[agent.sub_agents]]
# name = "translate-agent"
# description = "Specialized translation sub-agent"
# system_prompt_file = "~/.morph/prompts/translate.md"
# model = "gpt-4o"
# max_iterations = 5
# allowed_tools = ["text_gen", "text_qa"]
# disabled_tools = []
# context_policy = "task_only"
# merge_strategy = "raw"
# output_schema = ""
```

> **默认值说明**：
> - `step_timeout_seconds` 默认 330（含 LLM 调用 + 工具执行）
> - `guardrail_enabled` 默认 true
> - `guardrail_max_retries` 默认 1
> - `timeout_seconds`（text model）默认 300
> - video provider 默认 volcengine

**API Key 解析优先级**：CLI flag > 环境变量（`api_key_env`）> 配置中 `api_key`（明文，**不推荐**，CLI 启动时警告）。系统 keyring（macOS Keychain / Linux libsecret）计划为 P1。

### 6.8 项目结构

```
morph/
├── CMakeLists.txt
├── README.md
├── REQUIREMENTS.md
├── AGENTS.md
├── config.toml.example
├── exts/
│   ├── demo-upper/
│   │   ├── manifest.toml
│   │   ├── upper.c
│   │   └── README.md
│   ├── demo-translate/
│   │   ├── manifest.toml
│   │   ├── translate.sh
│   │   └── README.md
│   ├── demo-guardrail-pii/
│   │   ├── manifest.toml
│   │   ├── pii_check.c
│   │   └── README.md
│   └── web-fetch/
│       ├── manifest.toml
│       ├── web-fetch.ts
│       ├── web-fetch.sh
│       └── package.json
├── skills/
│   └── code-review/
│       └── SKILL.md
├── misc/
│   └── demo.png
├── src/
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── cli.c
│   ├── cli.h
│   ├── config.c
│   ├── config.h
│   ├── session.c
│   ├── session.h
│   ├── vendor_toml_compat.h
│   ├── render/
│   │   ├── CMakeLists.txt
│   │   ├── markdown.c
│   │   ├── markdown.h
│   │   ├── highlight.c
│   │   ├── highlight.h
│   │   ├── latex_unicode.c
│   │   ├── latex_unicode.h
│   │   ├── image.c
│   │   ├── image.h
│   │   ├── video.c
│   │   └── video.h
│   ├── agent/
│   │   ├── CMakeLists.txt
│   │   ├── react.c
│   │   ├── react.h
│   │   ├── context.c
│   │   ├── context.h
│   │   ├── compress.c
│   │   ├── compress.h
│   │   ├── tokenizer.c
│   │   ├── tokenizer.h
│   │   ├── tool.c
│   │   ├── tool.h
│   │   ├── tool_context.c
│   │   ├── tool_context.h
│   │   ├── guardrail.c
│   │   ├── guardrail.h
│   │   ├── plan.c
│   │   ├── plan.h
│   │   ├── memory.c
│   │   ├── memory.h
│   │   ├── sub_agent.c
│   │   ├── sub_agent.h
│   │   ├── system_prompt.h
│   │   └── tools/
│   │       ├── CMakeLists.txt
│   │       ├── text_gen.c
│   │       ├── text_gen.h
│   │       ├── text_qa.c
│   │       ├── text_qa.h
│   │       ├── img_gen.c
│   │       ├── img_gen.h
│   │       ├── img_inpaint.c
│   │       ├── img_inpaint.h
│   │       ├── img_compose.c
│   │       ├── img_compose.h
│   │       ├── img_info.c
│   │       ├── img_info.h
│   │       ├── img_resize.c
│   │       ├── img_resize.h
│   │       ├── img_convert.c
│   │       ├── img_convert.h
│   │       ├── img_annotate.c
│   │       ├── img_annotate.h
│   │       ├── vid_gen.c
│   │       ├── vid_gen.h
│   │       ├── file_read.c
│   │       ├── file_read.h
│   │       ├── file_list.c
│   │       ├── file_list.h
│   │       ├── file_info.c
│   │       ├── file_info.h
│   │       ├── bash_exec.c
│   │       ├── bash_exec.h
│   │       ├── skill_activate.c
│   │       ├── skill_activate.h
│   │       ├── plan.c
│   │       ├── plan.h
│   │       ├── ask_user.c
│   │       ├── ask_user.h
│   │       ├── sub_agent_tools.c
│   │       └── sub_agent_tools.h
│   ├── skill/
│   │   ├── CMakeLists.txt
│   │   ├── skill.c
│   │   ├── skill.h
│   │   ├── skill_parse.c
│   │   └── skill_parse.h
│   ├── sandbox/
│   │   ├── CMakeLists.txt
│   │   ├── sandbox.c
│   │   ├── sandbox.h
│   │   ├── loader.c
│   │   └── loader.h
│   ├── ext/
│   │   ├── CMakeLists.txt
│   │   ├── ext.c
│   │   ├── ext.h
│   │   ├── manifest.c
│   │   └── manifest.h
│   ├── ipc/
│   │   ├── CMakeLists.txt
│   │   ├── jsonrpc.c
│   │   └── jsonrpc.h
│   ├── mcp/
│   │   ├── CMakeLists.txt
│   │   ├── mcp.h
│   │   ├── mcp_client.c
│   │   ├── mcp_http.c
│   │   └── mcp_stdio.c
│   ├── models/
│   │   ├── CMakeLists.txt
│   │   ├── llm.c
│   │   ├── llm.h
│   │   ├── image_gen.c
│   │   ├── image_gen.h
│   │   ├── video_gen.c
│   │   └── video_gen.h
│   ├── http/
│   │   ├── CMakeLists.txt
│   │   ├── client.c
│   │   ├── client.h
│   │   ├── sse.c
│   │   └── sse.h
│   ├── db/
│   │   ├── CMakeLists.txt
│   │   ├── database.c
│   │   └── database.h
│   ├── fastcgi/
│   │   ├── CMakeLists.txt
│   │   ├── README.md
│   │   ├── PATCHES.md
│   │   ├── main.c
│   │   ├── router.c
│   │   ├── router.h
│   │   ├── session_store.c
│   │   ├── session_store.h
│   │   ├── auth.c
│   │   ├── auth.h
│   │   ├── fcgi_io.c
│   │   ├── fcgi_io.h
│   │   ├── event_sink.c
│   │   ├── event_sink.h
│   │   ├── action_pump.c
│   │   ├── action_pump.h
│   │   ├── agent_bridge.c
│   │   └── handlers/
│   │       ├── handlers.h
│   │       ├── health.c
│   │       ├── sessions.c
│   │       ├── turns.c
│   │       ├── canvas.c
│   │       ├── actions.c
│   │       └── events.c
│   └── util/
│       ├── CMakeLists.txt
│       ├── arena.c
│       ├── arena.h
│       ├── file.c
│       ├── file.h
│       ├── log.c
│       ├── log.h
│       ├── spin.c
│       ├── spin.h
│       ├── utf8.c
│       ├── utf8.h
│       ├── base64.c
│       ├── base64.h
│       ├── image_util.c
│       ├── image_util.h
│       ├── error.c
│       ├── error.h
│       ├── bpe.c
│       └── bpe.h
├── vendor/
│   ├── cJSON.c
│   ├── cJSON.h
│   ├── toml.c
│   ├── toml.h
│   ├── stb_image.h
│   ├── stb_image_write.h
│   ├── stb_image_resize2.h
│   ├── sheredom_utf8.h
│   └── tiktoken/
│       ├── cl100k_base.tiktoken
│       └── o200k_base.tiktoken
└── tests/
    ├── CMakeLists.txt
    ├── test_arena.cpp
    ├── test_log.cpp
    ├── test_spin.cpp
    ├── test_sse.cpp
    ├── test_database.cpp
    ├── test_tool.cpp
    ├── test_context.cpp
    ├── test_config.cpp
    ├── test_file.cpp
    ├── test_tokenizer.cpp
    ├── test_session.cpp
    ├── test_http.cpp
    ├── test_text_gen.cpp
    ├── test_image.cpp
    ├── test_compress.cpp
    ├── test_react.cpp
    ├── test_markdown.cpp
    ├── test_skill.cpp
    ├── test_bash_exec.cpp
    ├── test_render.cpp
    ├── test_memory.cpp
    ├── test_mcp.cpp
    ├── test_ask_user.cpp
    ├── test_sandbox.cpp
    ├── test_tool_context.cpp
    ├── test_sub_agent.cpp
    ├── systest.sh
    └── test_ext_demo.c
```

### 6.9 关键接口设计

#### 6.9.0 内存管理（Arena）

全局采用 Arena 线性分配器替代逐个 `malloc`/`free`，从架构层面消除内存泄漏与 double-free 风险。

**设计要点**：
- Bump-pointer 分配，内存零初始化（`memset`），对齐至 `sizeof(void *)`
- 单个 Arena 由带头链表的 region 组成；当前 region 不够时自动链入新 region（同容量或按需扩容）
- 默认 region 大小 64KB（内部常量），可按需指定
- 生命周期：scope 级创建（`arena_create`），scope 退出时整体销毁（`arena_destroy`）；无需逐个释放
- `arena_reset` 释放溢出 region 并重置主 region，允许复用同一 arena 实例
- 所有核心结构体（`react_context`、`chat_response` 等）持有 `struct arena *`，内部字符串与动态数据均从 arena 分配

**与裸 `malloc`/`free` 的关系**：仅 arena 内部的 region 创建/销毁、以及少量短生命周期缓冲使用 `malloc`/`free`；业务代码优先通过 `arena_alloc`/`arena_strdup`，或下述 `morph_buf`/`morph_array` 等基础容器分配，避免散落的手动内存管理。

```c
struct arena {
	char *buf;		/* 当前 region 缓冲区 */
	size_t cap;		/* 当前 region 容量 */
	size_t used;		/* 当前 region 已用字节 */
	struct arena *next;	/* 溢出 region 链表 */
};

struct arena *arena_create(size_t cap);		/* 0 使用默认大小 */
void arena_destroy(struct arena *a);		/* 递归释放所有 region */
void *arena_alloc(struct arena *a, size_t size);		/* 对齐分配，零填充 */
void *arena_alloc_aligned(struct arena *a, size_t size, size_t align);
void arena_reset(struct arena *a);		/* 释放溢出 region，重置主 region */
char *arena_strdup(struct arena *a, const char *s);		/* arena 内字符串复制 */
```

> **KPI 关联**：Arena 批量释放保证 Valgrind `0 definitely lost`（§3）；单次 `arena_destroy` 代替 N 次 `free`，降低空闲内存碎片，支撑「空闲常驻 < 15MB」指标。

#### 6.9.0a 基础数据结构（`src/util/`）

所有共享的基础容器统一收敛在 `src/util/`（随 `morph-util` 链接），**业务代码必须复用，禁止重复造轮子**（自行实现可变缓冲、动态数组、哈希表等）。

| 结构 | 头文件 | 用途 | 关键 API |
|------|--------|------|----------|
| `struct arena` | `arena.h` | scope 级线性分配器 | `arena_create`/`arena_destroy`/`arena_alloc`/`arena_alloc_aligned`/`arena_strdup`/`arena_reset` |
| `morph_buf_t` | `buf.h` | 可增长字节/字符串构造器 | `morph_buf_init`(堆)/`morph_buf_init_arena`、`append`/`puts`/`putc`/`printf`/`vprintf`、`morph_buf_cstr`/`morph_buf_str`/`morph_buf_detach`、`morph_buf_cleanup` |
| `morph_array_t` | `array.h` | 泛型动态数组（init 指定元素大小） | `morph_array_init`(堆)/`morph_array_init_arena`、`push`/`push_n`/`pop`/`get`/`reserve`/`clear`、`morph_array_foreach`、`morph_array_cleanup` |
| `morph_strmap_t` | `strmap.h` | 开放寻址 string→`void *` 哈希表 | `morph_strmap_init`/`cleanup`/`clear`、`set`/`get`/`contains`/`remove`/`len` |
| `morph_str_t` | `str.h` | `{len, const char *}` 字符串视图（常 arena 背书） | `morph_strdup`/`strndup`、`morph_strcmp`/`strcasecmp`/`strncmp`、`morph_str_to_c`、`morph_str_chr`/`rchr`/`trim`、`MORPH_STRLIT` |
| `struct morph_queue` | `queue.h` | 侵入式双向链表（宏 + header-only，无 typedef） | `morph_queue_init`、`insert_head`/`insert_tail`、`remove`、`foreach`/`foreach_safe`、`morph_queue_data`、`sort`/`split`/`middle` |

**使用约定**：
- 变长字符串拼接一律用 `morph_buf`，**不得**用固定 `char[N]` + `snprintf` 累加。
- 数量不固定的集合用 `morph_array`，**不得**用固定容量 C 数组兜底（注意 `push` 可能触发 realloc，勿跨 push 持有元素指针）。
- 字符串为 key 的查找用 `morph_strmap`（如工具注册表）；整数 key 直接用 `morph_array` 下标或线性查找。
- `morph_str_t` 用于非拥有切片；cJSON 取值与多数 API 仍传普通 `const char *`。

```c
/* buf.h —— 可增长字符串构造 */
typedef struct {
	char         *data;
	size_t        len;
	size_t        cap;
	int           heap_alloc;
	int           failed;
	struct arena *arena;
} morph_buf_t;

/* array.h —— 泛型动态数组 */
typedef struct {
	void   *elts;
	size_t  nelts;
	size_t  cap;
	size_t  size;		/* 元素字节大小 */
	int     heap_alloc;
} morph_array_t;

/* strmap.h —— string -> void* 哈希表 */
typedef struct {
	struct morph_strmap_entry *entries;
	size_t count;
	size_t cap;
	size_t deleted;
} morph_strmap_t;

/* str.h —— 字符串视图 */
typedef struct {
	size_t      len;
	const char *data;
} morph_str_t;
```

#### 6.9.1 ReAct 循环

```c
/* ReAct 步骤类型 */
enum react_step_type {
	REACT_STEP_THOUGHT,
	REACT_STEP_ACTION,
	REACT_STEP_OBSERVATION,
	REACT_STEP_REFLECTION,
	REACT_STEP_FINAL,
};

/* ReAct 状态机 */
enum react_state {
	REACT_STATE_INIT,
	REACT_STATE_THINKING,
	REACT_STATE_ACTING,
	REACT_STATE_OBSERVING,
	REACT_STATE_GUARDRAIL,
	REACT_STATE_FINAL,
	REACT_STATE_DONE,
	REACT_STATE_ABORT,		/* 超过 max_iterations */
	REACT_STATE_TOOL_FAIL,		/* 工具失败，回灌错误 */
};

/* 单个 ReAct 步骤 */
struct react_step {
	enum react_step_type type;
	char *content;
	char *tool_name;
	char *tool_args;
	char *tool_call_id;		/* Function Calling 的 tool_call ID */
	struct react_step *next;
};

/* Human-in-the-Loop (HITL) 审批机制 */
#define HITL_TOOLS_MAX 32
#define HITL_TOOL_NAME_MAX 64
#define HITL_AUTO_APPROVED_MAX 32

enum hitl_verdict {
	HITL_APPROVE,
	HITL_DENY,
	HITL_ALWAYS,		/* 本次审批后，该工具自动批准 */
};

typedef enum hitl_verdict (*hitl_approval_cb)(const char *tool_name,
					      const char *tool_args,
					      void *user_data);

struct hitl_config {
	int enabled;
	char tools[HITL_TOOLS_MAX][HITL_TOOL_NAME_MAX];  /* 需审批的工具列表 */
	int tools_count;
	int auto_approve_readonly;		/* 自动批准只读工具 */
	hitl_approval_cb approval_cb;
	void *approval_user_data;
	char auto_approved[HITL_AUTO_APPROVED_MAX][HITL_TOOL_NAME_MAX];
	int auto_approved_count;
};

/* FastCGI Action 机制 */
struct react_action {
	const char *type;          /* "approve" | "reject" | "cancel" | "prompt" */
	const char *payload_json;
};

typedef int (*react_action_drain_fn)(void *user, struct react_action *out,
				     int timeout_sec);

/* ReAct 循环上下文 */
struct react_context {
	struct react_step *steps;
	int step_count;
	int max_iterations;
	int step_timeout_seconds;
	int tool_max_retries;
	struct guardrail_config guardrail;
	int guardrail_retry_count;
	struct hitl_config hitl;
	ask_user_callback_fn ask_user_fn;
	void *ask_user_data;
	struct tool_registry *tools;
	struct message_list *messages;
	struct tokenizer *tokenizer;
	struct compress_config compress;
	void *llm_model;		/* struct model *（不透明指针） */
	char *final_answer;
	enum react_state state;
	char *tool_fail_name;
	char *tool_fail_args;
	int tool_fail_count;
	int empty_round_count;
	volatile sig_atomic_t cancelled;
	struct arena *arena;
	struct arena *session;		/* 会话级 arena（跨轮次持久） */
	char *system_prompt;
	char *memory_context;		/* 记忆上下文注入 */
	struct skill_registry *skills;
	char *workdir;
	react_action_drain_fn action_drain_fn;
	void *action_drain_user_data;
	int sub_agent_depth;		/* 子代理嵌套深度 */
	struct {
		char name[64];
		char description[256];
	} *sub_agent_info;
	int sub_agent_info_count;
};

/* ReAct 输出回调（用于 CLI Spinner 驱动） */
typedef int (*react_output_cb)(enum react_step_type type,
			       const char *content, void *user_data);

/* ReAct 循环上下文创建与销毁 */
struct react_context *react_context_create(struct tool_registry *tools,
					   struct tokenizer *tok,
					   struct compress_config *cfg,
					   struct guardrail_config *gcfg);
void react_context_destroy(struct react_context *ctx);
void react_reset(struct react_context *ctx);

/* FastCGI 会话工厂 */
struct session_store;
struct react_context *
react_context_create_for_session(struct session_store *store,
				 const char *session_id,
				 const char *user_id);

/* ReAct 循环执行 */
int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data);

/* 取消当前 ReAct 循环（Ctrl-C 触发） */
void react_cancel(struct react_context *ctx);
void react_cancel_active(void);
extern volatile sig_atomic_t react_sigint_flag;

/* 记忆上下文 */
int react_set_memory_context(struct react_context *ctx, const char *memory_context);

/* 活跃 ReAct 实例管理（子代理支持） */
int react_active_count(void);
void react_active_push(struct react_context *ctx);
void react_active_pop(struct react_context *ctx);

/* Action 机制（FastCGI 集成） */
int react_set_action_drain(struct react_context *ctx,
			   react_action_drain_fn fn, void *user);

/* HITL 辅助函数 */
int hitl_needs_approval(struct react_context *ctx, const char *tool_name);
void hitl_add_auto_approved(struct hitl_config *h, const char *tool_name);

/* 步骤创建与辅助 */
struct react_step *react_step_create(struct arena *arena,
				     enum react_step_type type,
				     const char *content,
				     const char *tool_name,
				     const char *tool_args,
				     const char *tool_call_id);
void react_step_destroy(struct react_step *step);
const char *react_step_type_name(enum react_step_type type);
const char *react_state_name(enum react_state state);
```

#### 6.9.2 工具系统

```c
#define TOOL_NAME_MAX 64
#define TOOL_DESC_MAX 512
#define TOOL_ARGS_SPEC_MAX 1024
#define TOOL_MAX_ENTRIES 64
#define TOOL_DISABLED_MAX 32

#define TOOL_FLAG_READONLY 0x01

struct tool_desc {
	char name[TOOL_NAME_MAX];
	char desc[TOOL_DESC_MAX];
	char args_spec[TOOL_ARGS_SPEC_MAX];
};

typedef int (*tool_exec_fn)(const char *args_json, char **result_json, void *user_data);
typedef void (*tool_user_data_destroy_fn)(void *user_data);

struct tool_entry {
	struct tool_desc desc;
	tool_exec_fn exec;
	void *user_data;
	tool_user_data_destroy_fn user_data_destroy;
	unsigned int flags;		/* TOOL_FLAG_READONLY 等 */
};

struct tool_registry {
	struct tool_entry entries[TOOL_MAX_ENTRIES];
	int count;
	char disabled[TOOL_DISABLED_MAX][TOOL_NAME_MAX];
	int disabled_count;
};

void tool_registry_init(struct tool_registry *reg);
void tool_registry_cleanup(struct tool_registry *reg);
int tool_register(struct tool_registry *reg, const char *name, const char *desc,
		  const char *args_spec, tool_exec_fn exec, void *user_data,
		  tool_user_data_destroy_fn user_data_destroy);
struct tool_entry *tool_lookup(struct tool_registry *reg, const char *name);
int tool_exec(struct tool_registry *reg, const char *name,
	      const char *args_json, char **result_json);
void tool_entry_cleanup_user_data(struct tool_registry *reg);
int tool_disable(struct tool_registry *reg, const char *name);
int tool_is_disabled(struct tool_registry *reg, const char *name);
int tool_is_readonly(struct tool_registry *reg, const char *name);
```

#### 6.9.3 模型接口

```c
struct arena;

typedef int (*sse_callback)(const char *token, void *user_data);

struct tool_desc;

struct tool_call {
	char id[128];
	char name[64];
	char *arguments;
};

struct chat_message {
	char *role;
	char *content;
	char *tool_call_id;
	struct tool_call *tool_calls;
	int tool_call_count;
};

struct chat_response {
	char *content;
	struct tool_call *tool_calls;
	int tool_call_count;
	struct arena *arena;
};

struct model {
	char name[64];
	char provider[32];
	char api_base[256];
	char api_key[256];
	char model_id[128];
	int context_limit;
	int max_tokens;		/* 单次响应最大生成 token 数 */
	long timeout_seconds;
	void *handle;
	int (*chat)(struct model *self, struct arena *arena,
		    const char *system_prompt,
		    const char **messages, int n,
		    sse_callback cb, void *user_data);
	int (*chat_with_tools)(struct model *self, struct arena *arena,
			       const char *system_prompt,
			       struct chat_message *messages, int msg_count,
			       struct tool_desc *tools, int tool_count,
			       struct chat_response *response,
			       sse_callback thought_cb, void *thought_ud);
	int (*generate)(struct model *self, const char *prompt,
			const char *out_path);
	void (*destroy)(struct model *self);
};

struct model *model_llm_create(const char *provider, const char *model_id,
			       const char *api_base, const char *api_key);
void model_destroy(struct model *m);

void chat_response_free(struct chat_response *resp);
void chat_message_cleanup(struct chat_message *msg, struct arena *arena);
void tool_call_cleanup(struct tool_call *tc, struct arena *arena);
```

#### 6.9.4 内置工具列表

| 工具名 | 功能 | 参数 | 对应模型 | 类型 | 只读 |
|--------|------|------|---------|------|------|
| text_gen | 文字内容生成 | prompt, style, length | LLM | 内置 | 否 |
| text_qa | 文字问答/改写 | prompt, context | LLM | 内置 | 是 |
| img_gen | 图片生成 | prompt, style, size, reference_image | DALL-E / SD / Volcengine | 内置 | 否 |
| img_inpaint | 区域生成(bbox+label) | annotation, prompt | 图像模型 i2i (确定性百分比指令) | 内置 | 否 |
| img_compose | 跨图融合(arrow+label) | annotation, prompt | 本地预合成 + 图像模型 i2i | 内置 | 否 |
| img_resize | 图片缩放 | file_path, width, height | stb_image_resize2 | 内置 | 否 |
| img_convert | 图片格式转换 | file_path, format | stb_image + stb_image_write | 内置 | 否 |
| img_info | 图片信息 | file_path | stb_image | 内置 | 是 |
| img_annotate | 图片标注 | file_path, annotations | 本地 | 内置 | 否 |
| vid_gen | 视频生成 | prompt, image_path?, duration, style | Volcengine / 可灵 / 即梦 | 内置 | 否 |
| file_read | 读取文本文件 | path, offset, limit | 本地 | 内置 | 是 |
| file_list | 列出目录内容 | path | 本地 | 内置 | 是 |
| file_info | 文件元数据 | path | 本地 | 内置 | 是 |
| bash_exec | 执行 shell 命令 | command | 本地（含黑名单过滤） | 内置 | 否 |
| plan | 创建/管理多步计划 | command, name, goal, steps | LLM | 内置 | 是 |
| ask_user | 向用户提问并等待回答 | question, choices | 本地 | 内置 | 是 |
| skill_activate | 激活 Skill 注入上下文 | name | 本地 | 内置 | 是 |
| agent_delegate | 异步委派子代理任务 | agent_name, task | 本地 | 子代理 | 否 |
| agent_status | 查询子任务状态 | task_id | 本地 | 子代理 | 是 |
| agent_sync | 同步调用子代理 | agent_name, task | 本地 | 子代理 | 否 |
| agent_fanout | 并行扇出子代理任务 | agent_name, tasks, merge | 本地 | 子代理 | 否 |
| translate | 文本翻译 | text, target_lang | LLM | Ext | — |
| upper | 文本转大写 | text | 本地 | Ext | — |
| ... | 社区/自定义 Ext + MCP 远程工具 | 按 manifest/API 定义 | 按定义 | Ext/MCP | — |

#### 6.9.5 上下文压缩

上下文相关结构体定义在 `context.h`，压缩函数在 `compress.h`。

```c
typedef int (*summarize_fn)(const char *text, void *user_data, char **out);

struct compress_config {
	int max_context_tokens;
	int max_history_rounds;
	double summarize_threshold_ratio;	/* 默认 0.8 */
	double compress_target_ratio;		/* 默认 0.5 */
	summarize_fn summarize;			/* 摘要回调（由 react.c 内部提供） */
	void *summarize_user_data;
};

struct message_list {
	char *role;
	char *content;
	char **file_paths;
	int file_count;
	int token_count;
	int compressed;
	struct message_list *next;
};

struct key_info {
	char *key;
	char *value;
	struct key_info *next;
};

struct bpe_encoder;

struct tokenizer {
	char model_name[64];
	int context_limit;
	int (*count)(const char *text);
	struct bpe_encoder *encoder;	/* BPE 编码器（可选） */
};

struct compress_result {
	int original_tokens;
	int compressed_tokens;
	int messages_removed;
	int messages_summarized;
	char *summary;
	struct key_info *preserved;
};

/* 消息链表辅助 */
struct message_list *msg_list_create(struct arena *session, const char *role,
				      const char *content, int token_count);
void msg_list_append(struct message_list **head, struct message_list *msg);
void msg_list_destroy(struct message_list *head);
int msg_list_count(struct message_list *head);

/* 上下文管理 */
int context_token_count(struct message_list *head, struct tokenizer *tok);
int context_needs_compress(struct message_list *head, struct tokenizer *tok,
			   struct compress_config *cfg);

/* 压缩策略 */
int compress_sliding_window(struct message_list **head, int keep_rounds,
			    struct compress_result *result);
int compress_react_trace(struct message_list **head,
			 struct compress_result *result);
int compress_detect_react_cycles(struct message_list *head);
struct key_info *extract_key_info(struct message_list *head);
void key_info_free(struct key_info *head);
int compress_summarize(struct message_list **head, int keep_rounds,
		       summarize_fn fn, void *fn_user,
		       struct arena *session,
		       struct compress_result *result);
```

#### 6.9.6 Ext 系统

| 类型 | 格式 | 执行方式 | 隔离级别 |
|------|------|---------|---------|
| 原生 Ext | `ext.so` | dlopen 加载，隔离线程执行 | seccomp-bpf 限制系统调用 |
| 外部 Ext | `ext`（可执行） | fork+execvp，子进程执行 | namespace+seccomp+rlimit 全隔离 |
| Guardrail Ext | `ext.so` | dlopen 加载，Guardrail 规则回调 | 同原生 Ext |

```c
enum ext_purpose {
	EXT_PURPOSE_TOOL      = 0,
	EXT_PURPOSE_GUARDRAIL = 1,
};

struct ext_manifest {
	char name[64];
	char version[32];
	char description[256];
	char author[64];
	char type[16];			/* "so" 或 "exec" */
	enum ext_purpose purpose;
	char entry[128];		/* exec 可执行文件名; so 符号名 */
	char hook[32];			/* Guardrail hook 点 */
	char action_text[512];		/* Guardrail 失败时的修正指导 */
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	char **allowed_env;
	int allowed_env_count;
	int max_memory_mb;
	int max_cpu_seconds;
	int max_open_files;
	char *args_schema;
	char *output_schema;
};

struct ext {
	struct ext_manifest manifest;
	char path[PATH_MAX];
	void *dl_handle;		/* dlopen 句柄（.so 类型） */
	int (*run)(const char *args_json, char **result_json);
	char exec_path[PATH_MAX];	/* exec 类型子进程路径 */
	struct tool_desc tool_desc;	/* 注册到 Tool Registry */
	int enabled;
};

int ext_load(struct ext *ex, const char *dir_path);
int ext_unload(struct ext *ex);
int ext_run(struct ext *ex, const char *args_json, char **result_json);
void ext_user_data_destroy(void *user_data);
```

#### 6.9.7 Ext ↔ 主进程 IPC

**协议**：基于行的 JSON-RPC 2.0，主进程通过 `stdin/stdout` 与子进程双向通信（`.so` 类型在隔离线程内通过环形 buffer）。

**主进程 → Ext**（单次调用）：
```json
{"jsonrpc":"2.0","id":1,"method":"run","params":{"prompt":"hi","target_lang":"en"}}
```

**Ext → 主进程**（可选回调，用于流式输出与权限请求）：
```json
{"jsonrpc":"2.0","method":"log","params":{"level":"info","msg":"calling api..."}}
{"jsonrpc":"2.0","method":"stream","params":{"chunk":"He"}}
{"jsonrpc":"2.0","method":"request_permission","params":{"perm":"network"}}
```

**Ext → 主进程（最终结果）**：
```json
{"jsonrpc":"2.0","id":1,"result":{"text":"Hello"}}
{"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"upstream timeout"}}
```

```c
struct jsonrpc_request {
	int id;
	const char *method;
	const char *params_json;
};

struct jsonrpc_response {
	int id;
	char *result_json;
	int has_error;
	int error_code;
	char *error_message;
};

char *jsonrpc_build_request(const struct jsonrpc_request *req);
int jsonrpc_parse_response(const char *resp_str, struct jsonrpc_response *out);
void jsonrpc_response_free(struct jsonrpc_response *resp);
```

#### 6.9.8 沙箱机制

| 平台 | 机制 | 默认拒绝 | 按权限放开 |
|------|------|---------|-----------|
| Linux | seccomp-bpf + rlimit + landlock | 所有 syscall | 见权限位域 |
| Linux 强隔离（P2） | + unshare（NEWPID/NEWNET/NEWNS） | 网络/PID | 仅 PERM_NETWORK |
| macOS | sandbox-exec + .sb profile | 网络/文件 | 同上（P2，当前未实现 `sandbox_enter_darwin`） |

**最小 syscall 白名单**（默认）：
`read, write, mmap, munmap, mprotect, brk, exit, exit_group, futex, rt_sigaction, rt_sigprocmask, clock_gettime, getpid, gettid, close, fstat, lseek, poll, ppoll, nanosleep`

```c
#define EXT_PERM_NETWORK	(1 << 0)
#define EXT_PERM_FILESYS	(1 << 1)
#define EXT_PERM_EXEC		(1 << 2)
#define EXT_PERM_ENV		(1 << 3)

struct sandbox_config {
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	char **allowed_env;
	int allowed_env_count;
	int max_memory_mb;
	int max_cpu_seconds;
	int max_file_size_mb;
	int max_processes;
	int max_open_files;
};

int sandbox_enter(struct sandbox_config *cfg);
int sandbox_apply_seccomp(unsigned int permissions);
int sandbox_apply_rlimits(unsigned int permissions, int max_memory_mb,
			  int max_cpu_seconds, int max_file_size_mb,
			  int max_processes, int max_open_files);
int sandbox_apply_fs(const char **allowed_paths, int count,
		     unsigned int permissions);
int sandbox_apply_env(const char **allowed_env, int count,
		      unsigned int permissions);
```

#### 6.9.9 ReAct Function Calling

ReAct 循环使用 LLM 原生 Function Calling（而非文本解析 `Action: tool_name(args)` 格式）。LLM 返回 `tool_calls` 数组，每个元素包含 `function.name`、`function.arguments`（JSON）和 `id`。运行时将 `id` 回填到 `tool_call_id`，用于后续 `tool_role` 消息关联。

**优势**：
- 无需正则解析 LLM 自由文本，避免格式错误
- 原生支持多工具并行调用（LLM 可一次返回多个 `tool_calls`）
- 参数自动 JSON 结构化，减少解析歧义

**并行执行**：当 LLM 返回多个 `tool_calls` 时，每个工具调用在独立 pthread 中执行，通过 `async_tool_call` 结构跟踪完成状态，完成后逐一回灌 Observation。

**Human-in-the-Loop (HITL)**：当 `hitl_enabled` 时，工具执行前检查 `hitl_needs_approval()`。若需审批，调用 `approval_cb` 获取用户决定（`APPROVE`/`DENY`/`ALWAYS`）。`ALWAYS` 将该工具加入 `auto_approved` 列表，后续不再提示。只读工具（`TOOL_FLAG_READONLY`）在 `auto_approve_readonly` 时自动批准。

#### 6.9.10 可插拔 Guardrail 引擎

Guardrail 采用可插拔规则引擎架构，支持三种规则类型和三个 hook 点。

**规则类型**：

| 类型 | 实现方式 | 适用场景 |
|------|---------|---------|
| `GUARDRAIL_RULE_C` | 内置 C 函数 | 客观条件验证（空回答、工具全失败等） |
| `GUARDRAIL_RULE_LLM` | LLM 评估 | 主观质量评估（输出是否满足用户意图） |
| `GUARDRAIL_RULE_EXT` | .so 动态库插件 | 自定义验证逻辑（PII 检测等） |

**Hook 点**：

| Hook | 触发时机 |
|------|---------|
| `GUARDRAIL_HOOK_INPUT` | 用户输入进入 ReAct 前 |
| `GUARDRAIL_HOOK_TOOL_OUTPUT` | 工具执行结果回灌前 |
| `GUARDRAIL_HOOK_OUTPUT` | LLM 最终输出返回用户前 |

```c
enum guardrail_verdict {
	GUARDRAIL_PASS = 0,
	GUARDRAIL_FAIL = 1,
};

enum guardrail_hook {
	GUARDRAIL_HOOK_INPUT,
	GUARDRAIL_HOOK_TOOL_OUTPUT,
	GUARDRAIL_HOOK_OUTPUT,
};

#define GUARDRAIL_RULES_MAX          16
#define GUARDRAIL_REASON_MAX         512
#define GUARDRAIL_ACTION_MAX         512
#define GUARDRAIL_NAME_MAX           64
#define GUARDRAIL_DESC_MAX           1024
#define GUARDRAIL_EXT_ENTRY_MAX      PATH_MAX

struct guardrail_eval_ctx {
	const char *user_input;
	const char *tool_name;
	const char *tool_args;
	const char *tool_result;
	const char *proposed_answer;
	const void *steps;
	int empty_round_count;
	struct arena *arena;
};

typedef enum guardrail_verdict (*guardrail_rule_fn)(
	const struct guardrail_eval_ctx *ctx,
	char *reason_out,
	size_t reason_cap);

enum guardrail_rule_type {
	GUARDRAIL_RULE_C,
	GUARDRAIL_RULE_LLM,
	GUARDRAIL_RULE_EXT,
};

enum guardrail_ext_type {
	GUARDRAIL_EXT_EXEC = 0,
	GUARDRAIL_EXT_SO   = 1,
};

typedef int (*guardrail_ext_check_fn)(const char *text,
				      const char *rule_name,
				      const char *description,
				      char **result_json);

struct guardrail_rule {
	char name[GUARDRAIL_NAME_MAX];
	enum guardrail_hook hook;
	enum guardrail_rule_type type;
	enum guardrail_ext_type ext_type;
	int enabled;
	guardrail_rule_fn check;
	void *dl_handle;
	guardrail_ext_check_fn ext_check;
	char description[GUARDRAIL_DESC_MAX];
	char ext_entry[GUARDRAIL_EXT_ENTRY_MAX];
	char action_text[GUARDRAIL_ACTION_MAX];
};

struct guardrail_result {
	enum guardrail_verdict verdict;
	char reason[GUARDRAIL_REASON_MAX];
	const struct guardrail_rule *triggered_rule;
};

struct model;

struct guardrail_config {
	int enabled;
	int max_retries;
	int max_empty_rounds;
	struct guardrail_rule rules[GUARDRAIL_RULES_MAX];
	int rule_count;
	struct model *llm;		/* LLM 评估规则使用的模型 */
};

/* 规则管理 */
int guardrail_rule_register(struct guardrail_config *cfg,
			     const char *name,
			     enum guardrail_hook hook,
			     enum guardrail_rule_type type,
			     guardrail_rule_fn check,
			     const char *description,
			     const char *ext_entry,
			     const char *action_text);
int guardrail_ext_so_load(struct guardrail_rule *rule);
void guardrail_ext_so_unload(struct guardrail_rule *rule);
int guardrail_rule_disable(struct guardrail_config *cfg, const char *name);
int guardrail_rule_enable(struct guardrail_config *cfg, const char *name);
struct guardrail_rule *guardrail_rule_lookup(struct guardrail_config *cfg,
					      const char *name);

/* 内置规则注册 */
void guardrail_register_builtin_rules(struct guardrail_config *cfg);

/* Hook 执行 */
struct guardrail_result guardrail_run_hook(const struct guardrail_config *cfg,
					   enum guardrail_hook hook,
					   const struct guardrail_eval_ctx *eval);

/* LLM 绑定 */
void guardrail_set_llm(struct guardrail_config *cfg, struct model *llm);
```

**内置规则**（`guardrail_register_builtin_rules` 自动注册）：

| 规则名 | Hook | 类型 | 验证条件 |
|--------|------|------|---------|
| all-tools-failed | output | C | 所有工具调用均返回错误 |
| creative-no-media | output | C | 创意工具被调用但无输出文件 |
| empty-answer | output | C | 回答为空或 `"(no response)"` |
| consecutive-empty | output | C | 连续空回答达到上限 |

**验证流程**：
1. LLM 返回无工具调用的文本 → 进入 `GUARDRAIL_HOOK_OUTPUT` hook
2. 遍历所有已启用的 output 规则，逐一执行 `check`
3. 任一规则返回 `GUARDRAIL_FAIL` → 构造 `action_text` 作为修正指导回灌
4. 所有规则通过 → 进入 Final

#### 6.9.11 长期记忆接口

```c
struct memory_options {
	int enabled;
	int hot_path_enabled;
	int cold_path_enabled;
	int llm_extract_enabled;
	int max_facts;
	int max_episodes;
	int max_procedures;
	int max_context_chars;
};

enum memory_clear_scope {
	MEMORY_CLEAR_ALL = 0,
	MEMORY_CLEAR_FACTS,
	MEMORY_CLEAR_EPISODES,
	MEMORY_CLEAR_PROCEDURES,
};

/* LLM 模型绑定（NULL 则回退到启发式提取） */
void memory_set_llm(struct model *llm);

/* 构建记忆上下文（注入 ReAct 系统提示） */
char *memory_build_context(struct db *db, int64_t session_id,
			   const char *query,
			   const struct memory_options *opts);

/* 渲染会话记忆摘要 */
char *memory_render_session(struct db *db, int64_t session_id,
			    int max_episodes);

/* 同步整合（当前轮次 → 记忆） */
int memory_consolidate_turn(struct db *db, int64_t session_id,
			    const char *user_input,
			    const char *assistant_output,
			    const struct react_step *steps,
			    int success,
			    const struct memory_options *opts);

/* 异步整合（后台线程，不阻塞主流程） */
int memory_consolidate_turn_async(struct db *db, int64_t session_id,
				  const char *user_input,
				  const char *assistant_output,
				  const struct react_step *steps,
				  int success,
				  const struct memory_options *opts);

/* 异步队列排空 + 线程回收 */
void memory_async_shutdown(void);

/* 清除记忆 */
int memory_clear(struct db *db, int64_t session_id,
		 enum memory_clear_scope scope);
```

#### 6.9.12 MCP 接口

```c
#define MCP_PROTOCOL_VERSION   "2025-06-18"
#define MCP_NAME_MAX           128
#define MCP_DESC_MAX           1024
#define MCP_SCHEMA_MAX         4096
#define MCP_URI_MAX            PATH_MAX
#define MCP_CMD_MAX            64
#define MCP_CMD_ARG_MAX        256
#define MCP_ENV_MAX            16
#define MCP_ENV_VAL_MAX        PATH_MAX
#define MCP_MAX_SERVERS        32
#define MCP_JSON_BUF_MAX       65536
#define MCP_READ_BUF_MAX       131072

enum mcp_transport_type {
	MCP_TRANSPORT_STDIO,
	MCP_TRANSPORT_STREAMABLE_HTTP,
};

struct mcp_tool_desc {
	char name[MCP_NAME_MAX];
	char title[MCP_NAME_MAX];
	char description[MCP_DESC_MAX];
	char input_schema[MCP_SCHEMA_MAX];
};

struct mcp_resource_desc {
	char uri[MCP_URI_MAX];
	char name[MCP_NAME_MAX];
	char description[MCP_DESC_MAX];
	char mime_type[64];
};

struct mcp_prompt_desc {
	char name[MCP_NAME_MAX];
	char description[MCP_DESC_MAX];
	char arguments_schema[MCP_SCHEMA_MAX];
};

struct mcp_server_config {
	char name[MCP_NAME_MAX];
	enum mcp_transport_type transport;
	/* stdio */
	char command[256];
	char cmd_args[MCP_CMD_MAX][MCP_CMD_ARG_MAX];
	int cmd_args_count;
	char env_keys[MCP_ENV_MAX][64];
	char env_vals[MCP_ENV_MAX][MCP_ENV_VAL_MAX];
	int env_count;
	/* http */
	char http_url[PATH_MAX];
	char http_auth_token_env[64];
	/* startup */
	int auto_connect;
	int connect_timeout;
};

struct mcp_client {
	struct mcp_server_config config;
	int connected;
	int connecting;
	char negotiated_version[32];
	char server_name[MCP_NAME_MAX];
	char server_version[64];
	int supports_tools;
	int supports_resources;
	int supports_prompts;
	int tools_list_changed;
	/* stdio transport */
	pid_t server_pid;
	int stdin_fd;
	int stdout_fd;
	/* http transport */
	void *curl_handle;
	char session_id[128];
	int next_req_id;
	pthread_mutex_t lock;
};

struct mcp_registry {
	struct mcp_client *servers[MCP_MAX_SERVERS];
	int count;
};

/* 注册表管理 */
void mcp_registry_init(struct mcp_registry *reg);
int mcp_registry_add(struct mcp_registry *reg, const struct mcp_server_config *cfg);
struct mcp_client *mcp_registry_get(struct mcp_registry *reg, const char *name);
int mcp_registry_count(struct mcp_registry *reg);
void mcp_registry_cleanup(struct mcp_registry *reg);

/* 生命周期 */
int mcp_connect_stdio(struct mcp_client *client);
int mcp_connect_http(struct mcp_client *client);
int mcp_initialize(struct mcp_client *client);
void mcp_disconnect(struct mcp_client *client);
int mcp_ensure_connected(struct mcp_client *client);

/* 工具 */
int mcp_list_tools(struct mcp_client *client, struct arena *arena,
		   struct mcp_tool_desc **out_tools, int *out_count);
int mcp_call_tool(struct mcp_client *client, struct arena *arena, const char *name,
		  const char *args_json, char **out_result_json);

/* 资源 */
int mcp_list_resources(struct mcp_client *client, struct arena *arena,
		       struct mcp_resource_desc **out_res, int *out_count);
int mcp_read_resource(struct mcp_client *client, struct arena *arena,
		      const char *uri, char **out_content);

/* 提示词 */
int mcp_list_prompts(struct mcp_client *client, struct arena *arena,
		     struct mcp_prompt_desc **out_prompts, int *out_count);
int mcp_get_prompt(struct mcp_client *client, struct arena *arena, const char *name,
		   const char *args_json, char **out_result);

/* 工具 */
int mcp_ping(struct mcp_client *client);

/* morph 工具注册表集成 */
int mcp_register_server_tools(struct mcp_client *client,
			      struct tool_registry *reg);
int mcp_register_server_resources(struct mcp_client *client,
				  struct tool_registry *reg);
int mcp_register_server_prompts(struct mcp_client *client,
				struct tool_registry *reg);
```

#### 6.9.13 子代理接口

```c
#define SUB_AGENT_TASK_MAX 8
#define SUB_AGENT_TASK_ID_MAX 32
#define SUB_AGENT_MAX_DEPTH 2

enum sub_agent_task_status {
	SUB_AGENT_PENDING,
	SUB_AGENT_RUNNING,
	SUB_AGENT_COMPLETED,
	SUB_AGENT_FAILED,
	SUB_AGENT_CANCELLED
};

struct sub_agent_entry {
	struct config_sub_agent cfg;
	char *system_prompt;
	struct model *llm;
};

struct sub_agent_task {
	char id[SUB_AGENT_TASK_ID_MAX];
	int agent_index;
	char *task_description;
	enum sub_agent_task_status status;
	char *result;
	int error_code;
	struct react_context *child_ctx;
	pthread_t thread;
	int joined;
	pthread_mutex_t mutex;
};

struct sub_agent_trace_event {
	char trace_id[36];
	char parent_trace_id[36];
	char agent_name[SUB_AGENT_NAME_MAX];
	int64_t start_ms;
	int64_t end_ms;
	char mode[16];
	int iteration_count;
	int token_usage;
	char *result_preview;
};

struct sub_agent_runtime {
	struct sub_agent_entry entries[SUB_AGENT_MAX];
	int entry_count;
	struct sub_agent_task tasks[SUB_AGENT_TASK_MAX];
	int task_count;
	int next_task_id;
	struct tool_registry *parent_tools;
	struct model *default_llm;
	struct tokenizer *tokenizer;
	struct compress_config *compress;
	int depth;
	char trace_file[PATH_MAX];
};

struct sub_agent_runtime *
sub_agent_runtime_create(struct tool_registry *parent_tools,
			 struct model *default_llm,
			 struct tokenizer *tokenizer,
			 struct compress_config *compress);
void sub_agent_runtime_destroy(struct sub_agent_runtime *rt);
int sub_agent_runtime_load_config(struct sub_agent_runtime *rt,
				  struct config_sub_agents *cfg);
struct sub_agent_entry *
sub_agent_find(struct sub_agent_runtime *rt, const char *name);
struct tool_registry *
sub_agent_build_tool_registry(struct sub_agent_runtime *rt,
			      struct sub_agent_entry *entry);
struct react_context *
sub_agent_create_context(struct sub_agent_runtime *rt,
			 struct sub_agent_entry *entry,
			 const char *task);
int sub_agent_invoke_sync(struct sub_agent_runtime *rt,
			  struct sub_agent_entry *entry,
			  const char *task, char **result);
int sub_agent_delegate(struct sub_agent_runtime *rt,
		       const char *agent_name, const char *task,
		       char **task_id_out);
int sub_agent_fanout(struct sub_agent_runtime *rt,
		     const char *agent_name,
		     const char **tasks, int task_count,
		     enum sub_agent_merge_strategy merge,
		     char **result);
int sub_agent_check_status(struct sub_agent_runtime *rt,
			   const char *task_id,
			   enum sub_agent_task_status *status_out,
			   char **result_out);
int sub_agent_apply_output_schema(const char *text,
				  const char *schema,
				  struct model *llm,
				  char **result);
void sub_agent_trace_write(struct sub_agent_runtime *rt,
			   struct sub_agent_trace_event *ev);
```

#### 6.9.14 Tool Context 接口

Tool Context 管理工具执行时的路径权限和命令审批。

```c
#define TOOL_CONTEXT_OUTPUT_DIR_MAX PATH_MAX
#define TOOL_CONTEXT_ALLOW_MAX 32
#define TOOL_CONTEXT_ALLOW_PATH_MAX PATH_MAX
#define TOOL_CONTEXT_COMMAND_MAX 1024

enum write_verdict {
	WRITE_DENY = 0,
	WRITE_ALLOW = 1,
	WRITE_ALWAYS = 2,
};

typedef enum write_verdict (*tool_write_approval_fn)(const char *path,
						     const char *output_dir,
						     void *user_data);

enum command_verdict {
	COMMAND_DENY = 0,
	COMMAND_ALLOW = 1,
	COMMAND_ALWAYS = 2,
};

typedef enum command_verdict (*tool_command_approval_fn)(
	const char *command, const char *cwd, void *user_data);

struct tool_context {
	char workdir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	char output_dir[TOOL_CONTEXT_OUTPUT_DIR_MAX];
	tool_write_approval_fn approval_fn;
	void *approval_user_data;
	char allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int allowed_dirs_count;
	tool_command_approval_fn command_approval_fn;
	void *command_approval_user_data;
	char allowed_commands[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_COMMAND_MAX];
	int allowed_commands_count;
	char exec_allowed_dirs[TOOL_CONTEXT_ALLOW_MAX][TOOL_CONTEXT_ALLOW_PATH_MAX];
	int exec_allowed_dirs_count;
};

struct tool_context *tool_context_create(const char *workdir,
					 const char *output_dir);
void tool_context_destroy(struct tool_context *tctx);
const char *tool_context_workdir(const struct tool_context *tctx);
const char *tool_context_output_dir(const struct tool_context *tctx);
int tool_context_check_write_path(struct tool_context *tctx, const char *path);
void tool_context_add_allowed_dir(struct tool_context *tctx, const char *dir);
void tool_context_set_command_approval(struct tool_context *tctx,
				       tool_command_approval_fn fn,
				       void *user_data);
int tool_context_allow_command(struct tool_context *tctx, const char *pattern);
int tool_context_allow_exec_dir(struct tool_context *tctx, const char *path);
int tool_context_check_command(struct tool_context *tctx,
			       const char *command, const char *cwd);
```

#### 6.9.15 Plan 子系统接口

```c
#define PLAN_NAME_MAX 64
#define PLAN_GOAL_MAX 512
#define PLAN_STEP_DESC_MAX 256
#define PLAN_MAX_STEPS 32
#define PLAN_MAX_PLANS 8
#define PLAN_STATUS_MAX 16

struct plan_step {
	int id;
	char description[PLAN_STEP_DESC_MAX];
	char status[PLAN_STATUS_MAX]; /* pending, in_progress, completed, failed, skipped */
};

struct plan {
	char name[PLAN_NAME_MAX];
	char goal[PLAN_GOAL_MAX];
	struct plan_step steps[PLAN_MAX_STEPS];
	int step_count;
	int active_step; /* index of current active step, -1 if none */
};

struct plan_registry {
	struct plan plans[PLAN_MAX_PLANS];
	int count;
};

void plan_registry_init(struct plan_registry *reg);
struct plan *plan_create(struct plan_registry *reg, const char *name,
			 const char *goal, const char **step_descs,
			 int step_count);
struct plan *plan_find(struct plan_registry *reg, const char *name);
int plan_update_step(struct plan_registry *reg, const char *plan_name,
		     int step_id, const char *status);
int plan_get_formatted(struct plan_registry *reg, char *buf, size_t buf_size);
```

#### 6.9.16 BPE Tokenizer 接口

```c
enum bpe_encoding {
	BPE_CL100K_BASE,	/* GPT-4 / GPT-3.5 */
	BPE_O200K_BASE,		/* GPT-4o */
};

struct bpe_encoder;

struct bpe_encoder *bpe_encoder_create(enum bpe_encoding encoding,
				       const char *vocab_dir);
void bpe_encoder_destroy(struct bpe_encoder *enc);
int bpe_count_tokens(struct bpe_encoder *enc, const char *text);
int bpe_count_tokens_n(struct bpe_encoder *enc, const char *text, size_t len);
```

> **词表文件**：`vendor/tiktoken/cl100k_base.tiktoken` 和 `vendor/tiktoken/o200k_base.tiktoken`，构建时复制到 build 目录。

---

### 6.10 Skill 系统

Skill 是热加载的指令包（`SKILL.md` 文件），可向 Agent 注入专业领域行为。与 Ext 不同，Skill 不引入新工具，而是通过系统提示扩展 Agent 的推理能力。

#### 6.10.1 Skill 发现

搜索路径（按顺序）：
1. `[skill] dir` 配置项（默认 `~/.morph/skills/`）
2. `~/.agents/skills/`（兼容标准路径）

每个子目录视为一个 Skill，需包含 `SKILL.md` 文件。

#### 6.10.2 Skill 解析（frontmatter）

`SKILL.md` 使用 YAML frontmatter 格式：

```markdown
---
name: code-review
description: Code review and analysis
license: MIT
compatibility: morph>=0.1
allowed_tools: text_gen,text_qa,file_read
metadata:
  author: morph-team
  version: "1.0"
---

(Skill instructions in Markdown — injected into system prompt when activated)
```

#### 6.10.3 Skill 激活

- **自动激活**：Skill 可设置 `auto_activate: true`，启动时自动注入
- **手动激活**：通过 `/skill activate <name>` 命令或 `skill_activate` 工具（由 ReAct 调用）
- **激活效果**：Skill 的 Markdown 内容以 `<skill name="xxx">` 标签包裹注入系统提示

#### 6.10.4 Skill 数据结构

```c
#define SKILL_NAME_MAX 65
#define SKILL_DESC_MAX 1025
#define SKILL_MAX_ENTRIES 64
#define SKILL_PATH_MAX PATH_MAX
#define SKILL_METADATA_MAX 16
#define SKILL_METADATA_KEY_MAX 64
#define SKILL_METADATA_VAL_MAX 256

struct skill_metadata_entry {
	char key[SKILL_METADATA_KEY_MAX];
	char value[SKILL_METADATA_VAL_MAX];
};

struct skill_frontmatter {
	char name[SKILL_NAME_MAX];
	char description[SKILL_DESC_MAX];
	char license[256];
	char compatibility[512];
	char allowed_tools[1024];
	struct skill_metadata_entry metadata[SKILL_METADATA_MAX];
	int metadata_count;
};

struct skill_entry {
	struct skill_frontmatter fm;
	char skill_dir[SKILL_PATH_MAX];
	char skill_md_path[SKILL_PATH_MAX];
	char *body;			/* SKILL.md 内容（frontmatter 之后） */
	int body_loaded;
	int enabled;
	int activated;
};

struct skill_registry {
	struct skill_entry entries[SKILL_MAX_ENTRIES];
	int count;
};

void skill_registry_init(struct skill_registry *reg);
void skill_registry_cleanup(struct skill_registry *reg);
int skill_discover(struct skill_registry *reg, const char *dir_path);
struct skill_entry *skill_lookup(struct skill_registry *reg, const char *name);
int skill_activate(struct skill_entry *skill);
void skill_deactivate(struct skill_entry *skill);
void skill_deactivate_all(struct skill_registry *reg);
char *skill_build_activated_instructions(struct skill_registry *reg);
int skill_build_catalog(struct skill_registry *reg, char *buf, size_t buf_size);
```

---

### 6.11 Spinner 动画系统

ReAct 过程中不逐行打印 Thought/Action/Observation，而是通过 Spinner 动画在终端单行内指示当前状态。

#### 6.11.1 Spinner 状态

| 状态 | 前缀 | 含义 |
|------|------|------|
| `SPIN_STATE_IDLE` | （空） | 空闲 |
| `SPIN_STATE_THINKING` | `>>` | LLM 推理中 |
| `SPIN_STATE_LOADING` | `~~` | 加载数据 |
| `SPIN_STATE_EXECUTING` | `->` | 执行工具 |
| `SPIN_STATE_DOWNLOADING` | `vv` | 下载数据 |
| `SPIN_STATE_UPLOADING` | `^^` | 上传数据 |
| `SPIN_STATE_COMPLETE` | `OK` | 完成 |
| `SPIN_STATE_ERROR` | `ERR` | 错误 |

#### 6.11.2 Spinner 样式

| 样式 | 帧序列 |
|------|--------|
| `SPIN_STYLE_DOTS`（默认） | ⠋⠙⠹⠸⠼⠴⠦⠧ |
| `SPIN_STYLE_ARROW` | ←↖↑↗→↘↓↙ |
| `SPIN_STYLE_PULSE` | ◐◓◑◒ |
| `SPIN_STYLE_BRAILLE` | ⠛⠟⠿⠻⠽⠾⠿⠾ |

#### 6.11.3 Spinner 渲染

```
⠋ Thinking → 流式预览文本... 3s
-> text_gen → prompt="..." 1s
OK Done (3s)
ERR Tool execution failed (1s)
```

- **帧间隔**：120ms
- **submessage**：附加在主消息后，通过 `→` 分隔；超长时自动水平滚动（2 字符/帧），最多 80 列可见
- **耗时**：运行阶段在右侧显示实时计时（`Ns` / `Nm Ns` / `Nh Nm`）；完成后在状态前缀后显示总耗时
- **流式 Thought 预览**：LLM 流式输出 token 时，取最后一行非空内容（最多 60 字符）作为 submessage 实时更新

#### 6.11.4 Spinner 线程模型

Spinner 在独立 pthread 中运行，主线程通过 mutex 保护状态更新：

```c
struct spin_context {
	enum spin_style style;
	enum spin_state state;
	char message[256];
	char submessage[512];		/* 附加信息（流式预览等） */
	FILE *output;
	int interval_ms;		/* 默认 120 */
	volatile int running;
	volatile int active;
	int frame;
	time_t start_time;
	time_t last_update;
	size_t last_render_width;	/* 上次渲染宽度（用于清除） */
	pthread_t thread;
	pthread_mutex_t mutex;
	volatile sig_atomic_t *cancel_flag;  /* 指向 react_sigint_flag */
};

void spin_init(struct spin_context *ctx, FILE *output);
void spin_start(struct spin_context *ctx, enum spin_state state, const char *message);
void spin_update(struct spin_context *ctx, const char *message);
void spin_set_sub(struct spin_context *ctx, const char *submessage);
void spin_stop(struct spin_context *ctx, enum spin_state final_state, const char *message);
void spin_pause(struct spin_context *ctx);
void spin_resume(struct spin_context *ctx);
void spin_render(struct spin_context *ctx);
void spin_clear(struct spin_context *ctx);
void spin_destroy(struct spin_context *ctx);
void spin_set_cancel_flag(struct spin_context *ctx, volatile sig_atomic_t *flag);
```

#### 6.11.5 CLI 输出回调映射

`output_callback` 将 ReAct 步骤类型映射到 Spinner 行为：

| 步骤类型 | Spinner 行为 |
|---------|-------------|
| `REACT_STEP_THOUGHT` | `spin_start(THINKING)` 或 `spin_set_sub(流式预览)` |
| `REACT_STEP_ACTION` | `spin_update(EXECUTING, tool_name)` + `spin_set_sub(tool_args)` |
| `REACT_STEP_OBSERVATION` | `spin_stop(COMPLETE/ERROR, 结果摘要)` |
| `REACT_STEP_REFLECTION` | 直接打印 `[Guardrail]` 行（验证失败原因） |
| `REACT_STEP_FINAL` | `spin_stop(COMPLETE)` + `markdown_render_ansi_with_media()` |

### 6.12 C 编码规范

- Tab 缩进（8 字符宽）；软上限 80，硬上限 100
- 函数名 `snake_case`，类型 `struct foo`；宏全大写
- 错误码使用负 `errno`（`-EINVAL` / `-ENOMEM`）或 `MORPH_ERR_*`（`src/util/error.h`）；错误返回必须使用 `MORPH_RETURN(code)` 而非裸 `return code;`
- `goto out;` 必须传播错误码：`int rc = 0; ... if ((rc = call()) < 0) goto out; ... out: return rc;`，禁止无条件 `return 0`
- 系统调用失败返回 `-errno`，禁止硬编码 `-EIO`
- 用户/LLM 面向消息使用 `morph_strerror(rc)`，禁止裸 `%d`
- C 风格注释，禁止 `//`
- `sizeof(var)` 而非 `sizeof(type)`
- 多语句宏 `do { } while (0)` 包裹
- 内存：统一通过 arena 分配（见 §6.9.0），禁止业务层直接 `malloc`/`free`；arena_alloc 失败返回 NULL，调用方 `goto out`
- 静态检查：`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`，CI 必须 0 warning

---

## 7. 错误处理

### 7.1 错误码体系

```c
typedef int morph_err_t;

enum morph_error {
	MORPH_ERR_NOT_CONFIGURED  = -257,
	MORPH_ERR_NOT_INITIALIZED = -258,
	MORPH_ERR_API             = -259,
	MORPH_ERR_NETWORK         = -260,
	MORPH_ERR_PARSE           = -261,
	MORPH_ERR_PROTOCOL        = -262,
	MORPH_ERR_DB              = -263,
	MORPH_ERR_FORMAT          = -264,
	MORPH_ERR_PROCESSING      = -265,
	MORPH_ERR_SANDBOX         = -266,
	MORPH_ERR_LOAD            = -267,
	MORPH_ERR_LLM             = -268,
};
```

### 7.2 错误处理宏

- `MORPH_RETURN(code)`：debug 模式日志 + 返回；release 模式直接返回
- `MORPH_SET_ERR(var, code)`：debug 模式日志 + 赋值；release 模式直接赋值
- `morph_strerror(err)`：返回所有错误码（POSIX + 自定义）的人类可读字符串

---

## 8. 日志与可观测性

| 项 | 设计 |
|------|------|
| 日志级别 | debug / info / warn / error，运行时可调 |
| 输出目的地 | stderr（交互期 warn+）+ 文件（全量，5MB rotate × 3） |
| ReAct 轨迹 | 写入 `react_traces` 表 + `--trace-file` 可导出 JSON |
| Token 用量 | 每次 LLM 调用记录 `prompt_tokens / completion_tokens / cost_estimate` |
| 工具调用埋点 | 工具名、耗时、成功/失败、参数摘要（脱敏） |
| 子代理追踪 | 子代理执行轨迹写入 trace 文件 |
| 错误上报 | 默认仅本地；可配置 `[telemetry]` 开关（opt-in） |
| 调试模式 | `MORPH_DEBUG=1` 打印每次 HTTP request/response |

---

## 9. 测试策略

| 层级 | 工具 | 覆盖目标 |
|------|------|---------|
| 单元 | GoogleTest（CMake FetchContent） | 核心模块 ≥ 70% |
| 集成 | Mock LLM（本地 HTTP server，返回固定 SSE 流） | ReAct 全流程 |
| 系统 | Shell 脚本（`systest.sh`） | 端到端 CLI 流程 |
| 模糊 | libFuzzer / AFL++ | manifest.toml 解析、JSON 解析（尚未实现） |
| 内存 | Valgrind + ASan + UBSan | 0 lost，0 UB |
| 沙箱逃逸 | 专项用例集（见下） | 100% 阻断 |
| 跨平台 | GitHub Actions：ubuntu-22.04，macos-14 | 冒烟全过 |

**测试文件清单**：

| 文件 | 测试模块 |
|------|---------|
| `test_arena.cpp` | Arena 内存分配器 |
| `test_log.cpp` | 日志系统 |
| `test_spin.cpp` | Spinner 动画 |
| `test_sse.cpp` | SSE 解析器 |
| `test_database.cpp` | SQLite 数据库 |
| `test_tool.cpp` | 工具注册表 |
| `test_context.cpp` | 上下文管理 |
| `test_config.cpp` | 配置解析 |
| `test_file.cpp` | 文件工具 |
| `test_tokenizer.cpp` | Token 计数 |
| `test_session.cpp` | 会话管理 |
| `test_http.cpp` | HTTP 客户端 |
| `test_text_gen.cpp` | 文字生成工具 |
| `test_image.cpp` | 图片处理 |
| `test_compress.cpp` | 上下文压缩 |
| `test_react.cpp` | ReAct 循环 |
| `test_markdown.cpp` | Markdown 渲染 |
| `test_skill.cpp` | Skill 系统 |
| `test_bash_exec.cpp` | Shell 执行工具 |
| `test_render.cpp` | 渲染系统 |
| `test_memory.cpp` | 长期记忆 |
| `test_mcp.cpp` | MCP 客户端 |
| `test_ask_user.cpp` | 用户交互工具 |
| `test_sandbox.cpp` | 沙箱机制 |
| `test_tool_context.cpp` | 工具执行上下文 |
| `test_sub_agent.cpp` | 子代理系统 |
| `systest.sh` | 端到端系统测试 |
| `test_ext_demo.c` | Ext demo（未纳入 CMake 构建） |

**沙箱逃逸用例**（必须 100% 阻断）：
1. Ext 调用 `execve("/bin/sh", ...)` 而未声明 `exec` 权限
2. Ext 打开 `/etc/shadow` 而未声明对应 `filesystem` 路径
3. Ext `socket(AF_INET, ...)` 而未声明 `network`
4. Ext `fork()` 炸弹超过 rlimit
5. Ext 内存分配超过 `max_memory_mb` → 被 OOM 杀掉，主进程不崩
6. Ext 死循环超过 `max_cpu_seconds` → 被 SIGKILL

---

## 10. 非功能性需求

| 维度 | 要求 |
|------|------|
| 响应时间 | 首 token < 1s（P95）；图片 < 30s；视频异步 |
| 编译体积 | 动态链接 < 8MB |
| 内存 | 空闲 < 15MB；对话峰值 < 80MB |
| 安装 | `cmake -B build && cmake --build build && cmake --install build` |
| 跨平台 | macOS 14+ / Ubuntu 22.04+ / Arch Linux 优先；Windows（MinGW）P3 |
| 离线 | 无网时可浏览历史与已生成产物 |
| 安全 | API Key 文件 0600；不上传用户数据；Ext 沙箱最小权限 |
| 扩展 | 新增模型仅需实现 `struct model` 接口 |
| 编译器 | GCC ≥ 10 / Clang ≥ 14，C11 |
| 国际化 | 内置中英双语 prompt 与帮助文案；可通过 `LANG` 切换 |
| 无障碍 | `--no-color` 禁用 ANSI，便于屏幕阅读器 |

---

## 11. 里程碑

| 里程碑 | 周次 | 交付物 | 状态 |
|--------|------|--------|------|
| **M1 / MVP** | W1–W4 | 项目骨架 + CLI + 文字对话（流式）+ 会话持久化 + Token 计数 + 滑动窗口 + 1 个 demo Ext（无沙箱） | **已完成** |
| **M2 / V0.2** | W5–W7 | 文生图 + 图片理解 + 终端预览（kitty/sixel/iterm2） | **已完成** |
| **M3 / V0.3** | W8–W10 | 文/图生视频 + mpv 播放 + 视频理解 + 异步轮询 + BPE Tokenizer + 长期记忆 + 子代理 + 可插拔 Guardrail + MCP + Plan + FastCGI | **已完成**（v0.3.0） |
| M4 / V0.4 | W11–W13 | Ext 沙箱完善（seccomp+rlimit+landlock）+ install/enable/disable + macOS sandbox-exec | 进行中 |
| M5 / V0.5 | W14–W15 | 跨模态联动模板 + 摘要压缩完善 + 关键信息提取 + 递归摘要 | 待开始 |
| M6 / V1.0 | W16–W18 | 多模型切换 + Ext 市场（git）+ Homebrew formula + 模糊测试 | 待开始 |

> **M3 提前实现项**：BPE Tokenizer（原计划 M5/P2）、长期记忆系统（原计划 M5）、子代理系统（原计划 M6）、可插拔 Guardrail 引擎（原计划 M4）、MCP 客户端（新增需求）、Plan 子系统（新增需求）、FastCGI 前端（新增需求）。

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解 | 截止 |
|------|------|------|------|
| 第三方 API 不稳定 | 服务中断 | 多供应商 + 自动降级 | M2 |
| 视频生成成本高 | 运营成本 | 用量提示 + 本地缓存复用 | M3 |
| 内容合规风险 | 平台风险 | 输出过滤 + 用户自担提示 | M3 |
| 终端兼容性 | 图片显示异常 | 三协议探测 + 路径回退 | M2 |
| C 开发效率 | 进度滞后 | 单文件库优先 + 接口抽象 + CI 强约束 | M1 |
| 内存管理 | 泄漏/崩溃 | Arena + Valgrind/ASan in CI | M1 |
| Ext 安全 | 恶意 Ext | 沙箱 + 最小权限 + 审计 | M4 |
| 沙箱逃逸 | 系统安全 | 专项测试集 + 安全评审 | M4 |
| 摘要丢关键信息 | 上下文劣化 | 关键信息强制保留 + 用户阈值可调 | M5 |
| Token 计数误差 | 截断/浪费 | BPE + 保守估算 + 分批 | M1（BPE 已在 M3 实现） |
| 静态体积超 8MB | 不达 KPI | 动态链接 + LTO + strip + 删除未使用代码 | M1 |
| Ext IPC 性能 | 流式卡顿 | 行缓冲 + 共享内存（P2） | M4 |
| MCP 服务器不稳定 | 远程工具不可用 | 延迟连接 + 超时控制 + 优雅降级 | M3 |
| 记忆系统准确性 | 错误记忆注入 | LLM 提取 + 置信度 + 用户可清除 | M3 |
| 子代理递归 | 资源耗尽 | 最大深度 2 层限制 | M3 |

---

## 13. 开放问题

| # | 问题 | 状态 | 决策建议 |
|---|------|------|---------|
| 1 | 产品命名 | **Decided**：morph | — |
| 2 | 优先接入哪些 API | **Decided**（M1）：OpenAI + Volcengine | — |
| 3 | 是否支持本地模型（Ollama） | Open | P2 评估 |
| 4 | SQLite 是否硬依赖 | **Decided**：是 | 简化数据层 |
| 5 | 终端图片是否 fallback feh/sxiv | **Decided**：否，只回退路径 | 减少依赖 |
| 6 | 视频理解是否本地抽帧 | Open | M3 评测精度 vs 成本 |
| 7 | 是否发布 Homebrew/AUR | M6 决策 | — |
| 8 | Ext 是否需 NetNS 隔离 | **Decided**：P2 | 默认 seccomp 即可 |
| 9 | `.so` Ext 是否需符号可见性限制 | **Decided**：是 | 仅导出 `ext_run` |
| 10 | MCP 协议版本跟进策略 | **Decided**：跟踪最新稳定版（当前 2025-06-18） | — |
| 11 | FastCGI 是否需要 WebSocket 支持 | Open | SSE 已满足流式需求 |
| 12 | 记忆系统是否需要跨用户共享 | **Decided**：否，会话级作用域 | 隐私优先 |
