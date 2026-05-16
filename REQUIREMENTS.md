# 多题材 Agent 需求文档

> **文档版本**: v0.3
> **状态**: Updated — 同步代码实际行为

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

### 1.4 非目标（Non-Goals）
- 不提供图形界面（GUI / Web UI）
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
| Token 计数 | 调用前精确估算，基于模型 tokenizer | P0 |
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

### 4.8 Ext 沙箱系统

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 自动发现 | 扫描目录，解析 manifest.toml | P0（裁剪后） |
| 注册到 Tool Registry | Ext 即 Tool | P0 |
| 沙箱执行（seccomp + rlimit） | Linux | P0 |
| 沙箱执行（sandbox-exec） | macOS | P1 |
| Ext 启停 | enable / disable | P1 |
| Ext 安装/卸载 | install / remove（本地路径） | P1 |
| 远程仓库拉取 | git clone | P2 |
| Namespace 隔离（PID/NET） | 强隔离 | P2 |

> **MVP 范围裁剪**：M1 的 Ext 子系统只交付**最小闭环**——发现 + 注册 + 基础 seccomp 沙箱。`enable/disable/remove`、远程拉取、macOS 沙箱推迟到 V0.4。

---

## 5. CLI 交互设计

### 5.1 启动与对话示例

```text
$ morph

morph v0.1  |  /help 查看命令

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

> **说明**：ReAct 过程中不再逐行打印 Thought/Action/Observation，而是通过 Spinner 动画在单行内指示当前状态（见 §6.11）。仅在 `/trace` 命令中展示完整步骤列表。Guardrail 验证失败时会直接打印 `[Guardrail]` 标签行。

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
| 文字（Final） | 自实现 Markdown→ANSI（基于 md4c）+ 媒体内联回调 | 颜色 / 粗体 / 代码块；Markdown 中的 `![image](path)` / `![video](path)` 自动触发渲染 |
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
| `/switch <name\|id\|display_id>` | `/s` | 切换会话 | `/s abc1` |
| `/list` | `/ls` | 会话列表（含 display_id） | — |
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
| `/video <path>` | `/vid` | 注入视频（M3） | `/vid ./clip.mp4` |
| `/ext list` | `/x list` | 已注册工具列表 | — |
| `/ext info <name>` | — | 工具详情 | `/ext info text_gen` |
| `/ext install <path>` | — | 本地路径安装（M4） | — |
| `/ext enable <name>` | — | 启用（M4） | — |
| `/ext disable <name>` | — | 禁用（M4） | — |
| `/ext remove <name>` | — | 卸载（M4） | — |
| `/skill list` | `/sk list` | 已发现 Skill 列表 | — |
| `/skill info <name>` | — | Skill 详情 | `/skill info code-review` |
| `/skill activate <name>` | — | 激活 Skill | `/skill activate code-review` |
| `/skill deactivate <name>` | — | 停用 Skill | `/skill deactivate code-review` |

> **提示符格式**：`[display_id] $ `，其中 `display_id` 为 4 字符短标识，在创建会话时自动生成，便于快速引用。

---

## 6. 技术架构

### 6.1 整体分层

```
┌─────────────────────────────────────────────┐
│  CLI 交互层（readline + ANSI + 终端图像）    │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  ReAct Agent 核心                           │
│  ┌──────────────────────────────────────┐   │
│  │ LLM 推理（Thought/Action/Obs/Final）│   │
│  ├──────────────────────────────────────┤   │
│  │ Tool Registry（内置 + Ext）        │   │
│  ├──────────────────────────────────────┤   │
│  │ Ext Sandbox（seccomp/rlimit/ns）  │   │
│  ├──────────────────────────────────────┤   │
│  │ Context Manager（token/压缩/摘要）  │   │
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
                        ↘ TOOL_FAIL → THINKING（重试，带错误上下文）
                        ↘ MAX_ITER  → ABORT（返回部分结果）
                        ↘ GUARDRAIL_FAIL → THINKING（Guardrail 验证失败，回灌原因重试）
```

#### 6.2.2 终止条件（必须全部实现）

1. LLM 返回无工具调用的文本响应 → 进入 Guardrail 验证
2. 步数达到 `max_iterations`（默认 10，可配置） → 返回最后一次 Observation 并标记 `aborted`
3. 单步耗时超过 `step_timeout_seconds`（默认 60s） → 中断该工具调用，生成失败 Observation 回灌
4. 用户按 `Ctrl-C` → 优雅取消，保存已完成步骤到会话
5. 当 `guardrail_enabled` 时，LLM 输出最终回答后进入 `GUARDRAIL` 状态，由客观条件验证结果质量；若 Guardrail 验证失败则回到 `THINKING` 重试（最多 `guardrail_max_retries` 次，默认 1）

#### 6.2.3 工具失败处理

- 工具返回非零 → 构造形如 `Observation: tool error: <msg>` 的观察项回灌给 LLM
- 同一工具连续失败 ≥ 3 次 → 强制 `Final` 并报错给用户

#### 6.2.4 Prompt 模板（完整）

```
You are a multi-modal content creation assistant.
Available tools:
{tool_descriptions}

Instructions:
- Use the provided tools to accomplish tasks.
- If you need to call a tool, use the function calling interface.
  You may also include a brief thought in your text response before calling tools.
- If no tool is needed, respond directly with your answer.
- If a tool fails twice with the same args, change strategy or respond directly.
- Maximum {max_iterations} tool-calling iterations.
```

> **注意**：morph 已从文本解析（`Thought: ... Action: ... Final: ...`）迁移到 OpenAI Function Calling。LLM 的结构化工具调用通过 `tool_calls` 字段传递，无需文本格式约束。

### 6.3 上下文压缩流程（层级 fallback）

```
LLM 调用前:
  1. tokenize(messages) → total_tokens
  2. if total_tokens < threshold → 直接发送，结束
  3. extract_key_info(messages) → preserve list
  4. compress_react_trace()        ← 先压本轮已完成的 ReAct 步骤
  5. if 仍超阈值: compress_multimedia_refs()
  6. if 仍超阈值: compress_sliding_window(keep=N rounds)
  7. if 仍超阈值: compress_summarize(早期对话 → 单条 summary)
  8. if 仍超阈值: compress_recursive(对 summary 再摘要)
  9. if 仍超阈值: compress_system_prompt(裁工具描述)
 10. inject(preserve list + 压缩后 messages) → LLM
```

> 关键修订：步骤之间是「层级 fallback」而非并列，且关键信息提取始终在压缩之前，避免被丢弃。

### 6.4 技术选型

| 组件 | 选型 | 必需 | 备注 |
|------|------|------|------|
| 构建系统 | CMake ≥ 3.20 | ✓ | 跨平台 + FetchContent |
| 编译器 | GCC ≥ 10 / Clang ≥ 14，C11 | ✓ | — |
| CLI 输入 | readline（GNU）/ editline（BSD） | ✓ | macOS 默认 editline |
| HTTP / SSE | libcurl（系统库） | ✓ | 工业级 |
| TLS | libcurl 内置（OpenSSL 或 GnuTLS） | ✓ | — |
| JSON | cJSON | ✓ | 单文件嵌入 |
| TOML | toml（tomlc99 fork） | ✓ | 单文件嵌入 |
| Markdown | md4c | ✓ | 单文件嵌入 |
| 图片解码/编码/缩放 | stb_image / stb_image_write / stb_image_resize2 | ✓ | 单头文件 |
| 终端图像 | libsixel（可选）+ 自实现 kitty/iterm2 协议 | 可选 | 缺失时回退路径 |
| Token 计数 | 自实现 Unicode-Aware 估算（tokenizer.c） | ✓ | BPE 推迟到 P2 |
| 异步 / 子进程 | fork+exec + waitpid | ✓ | 无需 libuv |
| 多线程 | pthreads | ✓ | — |
| 持久化 | sqlite3（系统库） | ✓ | 单文件 DB |
| 沙箱（Linux） | libseccomp + setrlimit + landlock（可选） | ✓ | 主平台 |
| 沙箱（macOS） | sandbox-exec（系统命令） | ✓ | 子进程方式 |
| 视频播放 | fork+exec `mpv`（系统二进制） | ✓ | 用户指定 |
| 抽帧（可选） | ffmpeg（系统二进制） | 可选 | 视频理解本地 fallback |
| 日志 | 自实现（stderr + rotating file） | ✓ | 见 §9 |

### 6.5 模型/API 接入策略

| 能力 | 推荐 API | 备注 |
|------|----------|------|
| 文字对话 | OpenAI GPT-4o / Claude 3.5 / DeepSeek | 多模型可切 |
| 文生图 | DALL-E 3 / Stable Diffusion API | — |
| 图片理解 | GPT-4o Vision / Claude Vision | — |
| 文生视频 | 可灵 / 即梦 / Runway | 国内优先 |
| 图生视频 | 可灵 / 即梦 / Pika | 国内优先 |
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

CREATE INDEX idx_messages_session  ON messages(session_id, created_at);
CREATE INDEX idx_traces_session    ON react_traces(session_id, round_no);
CREATE INDEX idx_outputs_session   ON outputs(session_id, created_at);
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
timeout_seconds = 60

[model.image]
provider = "openai"
model = "dall-e-3"
api_key_env = "OPENAI_API_KEY"

[model.video]
provider = "kling"
model = "kling-1.5"
api_key_env = "KLING_API_KEY"
poll_interval_seconds = 5
poll_timeout_seconds = 600

[react]
max_iterations = 10
step_timeout_seconds = 60
tool_max_retries = 3
guardrail_enabled = false
guardrail_max_retries = 1
guardrail_min_tool_calls = 1
guardrail_must_have_output = true
guardrail_max_empty_rounds = 2
disabled_tools = []

[context]
summarize_threshold_ratio = 0.8
compress_target_ratio = 0.5
keep_recent_rounds = 6

[render]
prefer_image_protocol = "auto"
mpv_args = ["--really-quiet"]

[skill]
dir = "~/.morph/skills"

[prompt]
system_prompt_file = ""
system_prompt_dir = ""

[ext]
dir = "~/.morph/exts"
default_max_memory_mb = 128
default_max_cpu_seconds = 30
```

**API Key 解析优先级**：CLI flag > 环境变量（`api_key_env`）> 系统 keyring（macOS Keychain / Linux libsecret，P1）> 配置中 `api_key`（明文，**不推荐**，CLI 启动时警告）。

### 6.8 项目结构

```
morph/
├── CMakeLists.txt
├── cmake/
│   └── FindModules/
├── README.md
├── REQUIREMENTS.md
├── config.example.toml
├── src/
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── cli.c
│   ├── cli.h
│   ├── config.c
│   ├── config.h
│   ├── session.c
│   ├── session.h
│   ├── render/
│   │   ├── CMakeLists.txt
│   │   ├── markdown.c
│   │   ├── markdown.h
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
│   │   ├── tools/
│   │   │   ├── CMakeLists.txt
│   │   │   ├── text_gen.c
│   │   │   ├── text_gen.h
│   │   │   ├── text_qa.c
│   │   │   ├── text_qa.h
│   │   │   ├── img_gen.c
│   │   │   ├── img_gen.h
│   │   │   ├── img_edit.c
│   │   │   ├── img_edit.h
│   │   │   ├── img_resize.c
│   │   │   ├── img_resize.h
│   │   │   ├── img_convert.c
│   │   │   ├── img_convert.h
│   │   │   ├── img_info.c
│   │   │   ├── img_info.h
│   │   │   ├── vid_gen.c
│   │   │   ├── vid_gen.h
│   │   │   ├── file_read.c
│   │   │   ├── file_read.h
│   │   │   ├── file_list.c
│   │   │   ├── file_list.h
│   │   │   ├── file_info.c
│   │   │   ├── file_info.h
│   │   │   ├── bash_exec.c
│   │   │   ├── bash_exec.h
│   │   │   ├── skill_activate.c
│   │   │   └── skill_activate.h
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
│       └── image_util.h
├── vendor/
│   ├── cJSON.c
│   ├── cJSON.h
│   ├── toml.c
│   ├── toml.h
│   ├── md4c.c
│   ├── md4c.h
│   ├── stb_image.h
│   ├── stb_image_write.h
│   ├── stb_image_resize2.h
│   └── base64.h
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
    └── test_bash_exec.cpp
```

### 6.9 关键接口设计

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

/* Guardrail 验证结果 */
enum guardrail_verdict {
	GUARDRAIL_PASS,
	GUARDRAIL_FAIL_NO_TOOLS,		/* 未调用工具但期望有输出 */
	GUARDRAIL_FAIL_NO_OUTPUT,		/* 回答中无生成产物 */
	GUARDRAIL_FAIL_EMPTY_ANSWER,		/* 空回答 */
	GUARDRAIL_FAIL_CONSECUTIVE_EMPTY,	/* 连续空回答 */
};

struct guardrail_result {
	enum guardrail_verdict verdict;
	char reason[512];
};

/* Guardrail 配置 */
struct guardrail_config {
	int enabled;
	int max_retries;
	int min_tool_calls;		/* 期望最少工具调用次数 */
	int must_have_output;		/* 回答中是否必须包含生成产物 */
	int max_empty_rounds;		/* 连续空回答上限 */
};

/* ReAct 循环上下文 */
struct react_context {
	struct react_step *steps;
	int step_count;
	int max_iterations;
	int step_timeout_seconds;
	int tool_max_retries;
	struct guardrail_config guardrail;
	int guardrail_retry_count;
	struct tool_registry *tools;
	struct message_list *messages;
	struct tokenizer *tokenizer;
	struct compress_config compress;
	void *llm_model;		/* struct model *（不透明指针） */
	char *final_answer;
	enum react_state state;
	char tool_fail_name[64];
	char tool_fail_args[512];
	int tool_fail_count;
	int empty_round_count;
	volatile sig_atomic_t cancelled;
	struct arena *arena;
	char *system_prompt;
	struct skill_registry *skills;
};

/* ReAct 输出回调（替代旧 sse_callback，用于 CLI Spinner 驱动） */
typedef int (*react_output_cb)(enum react_step_type type,
			       const char *content, void *user_data);

/* ReAct 循环执行 */
int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data);

/* 取消当前 ReAct 循环（Ctrl-C 触发） */
void react_cancel(struct react_context *ctx);

/* 重置当前轮次轨迹 */
void react_reset(struct react_context *ctx);
```

#### 6.9.2 工具系统

```c
struct tool_desc {
	const char *name;
	const char *desc;
	const char *args_spec;		/* JSON Schema */
};

typedef int (*tool_exec_fn)(const char *args_json,
			    char **result_json, void *user_data);

struct tool_entry {
	struct tool_desc desc;
	tool_exec_fn exec;
	void *user_data;
};

struct tool_registry {
	struct tool_entry entries[64];
	int count;
	char *disabled[32];
	int disabled_count;
};

int tool_register(struct tool_registry *reg,
		  const char *name, const char *desc,
		  const char *args_spec,
		  tool_exec_fn exec, void *user_data);
struct tool_entry *tool_lookup(struct tool_registry *reg, const char *name);
int tool_exec(struct tool_registry *reg, const char *name,
	      const char *args_json, char **result_json);
int tool_disable(struct tool_registry *reg, const char *name);
int tool_is_disabled(struct tool_registry *reg, const char *name);
```

#### 6.9.3 模型接口

```c
struct model {
	char name[64];
	char provider[32];
	char api_base[256];
	char api_key[256];
	char model_id[128];
	int  context_limit;
	int  timeout_seconds;
	void       *handle;
	int  (*chat)      (struct model *self, const char *prompt,
			   const char **messages, int n,
			   sse_callback cb, void *user_data);
	int  (*generate)  (struct model *self, const char *prompt,
			   const char *out_path);
	void (*destroy)   (struct model *self);
};

typedef int (*sse_callback)(const char *token, void *user_data);
```

#### 6.9.4 内置工具列表

| 工具名 | 功能 | 参数 | 对应模型 | 类型 |
|--------|------|------|---------|------|
| text_gen | 文字内容生成 | prompt, style, length | LLM | 内置 |
| text_qa | 文字问答/改写 | prompt, context | LLM | 内置 |
| img_gen | 图片生成 | prompt, style, size | DALL-E / SD | 内置 |
| img_edit | 图片编辑/理解 | prompt, file_path | GPT-4o Vision | 内置 |
| img_resize | 图片缩放 | file_path, width, height | stb_image_resize2 | 内置 |
| img_convert | 图片格式转换 | file_path, format | stb_image + stb_image_write | 内置 |
| img_info | 图片信息 | file_path | stb_image | 内置 |
| vid_gen | 视频生成 | prompt, file_path?, duration, style | 可灵 / 即梦 | 内置 |
| vid_qa | 视频理解 | file_path, question | GPT-4o / Gemini | 内置（P1） |
| file_read | 读取文本文件 | path, offset, limit | 本地 | 内置 |
| file_list | 列出目录内容 | path | 本地 | 内置 |
| file_info | 文件元数据 | path | 本地 | 内置 |
| bash_exec | 执行 shell 命令 | command | 本地（含黑名单过滤） | 内置 |
| skill_activate | 激活 Skill 注入上下文 | name | 本地 | 内置 |
| translate | 文本翻译 | prompt, target_lang | LLM | Ext |
| web_search | 网页搜索 | query | 外部 API | Ext |
| ... | 社区/自定义 Ext | 按 manifest 定义 | 按定义 | Ext |

#### 6.9.5 上下文压缩

```c
enum compress_policy {
	COMPRESS_SLIDING_WINDOW,
	COMPRESS_SUMMARIZE,
	COMPRESS_REACT_TRACE,
	COMPRESS_RECURSIVE,
};

struct compress_config {
	int max_context_tokens;
	int max_history_rounds;
	int summarize_threshold_ratio;	/* 默认 80% */
	int compress_target_ratio;	/* 默认 50% */
};

struct message {
	char *role;			/* system/user/assistant/observation */
	char *content;
	char **file_paths;
	int file_count;
	int token_count;
	int compressed;
	struct message *next;
};

struct key_info {
	char *key;
	char *value;
	struct key_info *next;
};

struct tokenizer {
	const char *model_name;
	int context_limit;
	int (*count)(const char *text);
};

struct compress_result {
	int original_tokens;
	int compressed_tokens;
	int messages_removed;
	int messages_summarized;
	char *summary;
	struct key_info *preserved;
};

int context_token_count(struct message *head, struct tokenizer *tok);
int context_needs_compress(struct message *head, struct tokenizer *tok,
			    struct compress_config *cfg);
int compress_sliding_window(struct message *head, int keep_rounds,
			    struct compress_result *result);
int compress_summarize(struct message *head, struct model *llm,
		       struct tokenizer *tok, struct compress_config *cfg,
		       struct compress_result *result);
int compress_react_trace(struct message *head,
			 struct compress_result *result);
struct key_info *extract_key_info(struct message *head);
int compress_recursive(struct message *head, struct model *llm,
		       struct tokenizer *tok, struct compress_config *cfg,
		       struct compress_result *result);
int compress_system_prompt(char *system_prompt, int target_tokens,
			  struct tokenizer *tok);
```

#### 6.9.6 Ext 系统

| 类型 | 格式 | 执行方式 | 隔离级别 |
|------|------|---------|---------|
| 原生 Ext | `ext.so` | dlopen 加载，隔离线程执行 | seccomp-bpf 限制系统调用 |
| 外部 Ext | `ext`（可执行） | fork+execvp，子进程执行 | namespace+seccomp+rlimit 全隔离 |

```c
#define EXT_PERM_NETWORK    (1 << 0)
#define EXT_PERM_FILESYS    (1 << 1)
#define EXT_PERM_EXEC       (1 << 2)
#define EXT_PERM_ENV        (1 << 3)

struct ext_manifest {
	char *name;
	char *version;
	char *description;
	char *author;
	char *type;			/* "so" 或 "exec" */
	char *entry;			/* exec 可执行文件名; so 符号名 */
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	char **allowed_env;
	int allowed_env_count;
	int max_memory_mb;
	int max_cpu_seconds;
	char *args_schema;
	char *output_schema;		/* "string" 或 "json" */
};

struct ext {
	struct ext_manifest manifest;
	char *path;
	void *dl_handle;		/* dlopen 句柄（.so 类型） */
	int (*run)(const char *args_json, char **result_json);
	char *exec_path;		/* exec 类型子进程路径 */
	struct tool_desc tool_desc;	/* 注册到 Tool Registry */
	int enabled;
};

int ext_load(struct ext *sk, const char *dir_path);
int ext_unload(struct ext *sk);
int ext_run(struct ext *sk, const char *args_json, char **result_json);
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

#### 6.9.8 沙箱机制

| 平台 | 机制 | 默认拒绝 | 按权限放开 |
|------|------|---------|-----------|
| Linux | seccomp-bpf + rlimit + landlock | 所有 syscall | 见权限位域 |
| Linux 强隔离（P2） | + unshare（NEWPID/NEWNET/NEWNS） | 网络/PID | 仅 PERM_NETWORK |
| macOS | sandbox-exec + .sb profile | 网络/文件 | 同上 |

**最小 syscall 白名单**（默认）：
`read, write, mmap, munmap, mprotect, brk, exit, exit_group, futex, rt_sigaction, rt_sigprocmask, clock_gettime, getpid, gettid, close, fstat, lseek, poll, ppoll, nanosleep`

```c
struct sandbox_config {
	unsigned int permissions;
	char **allowed_paths;
	int allowed_paths_count;
	int max_memory_mb;
	int max_cpu_seconds;
};

int sandbox_enter(struct sandbox_config *cfg);
int sandbox_apply_seccomp(unsigned int permissions);
int sandbox_apply_rlimits(int max_memory_mb, int max_cpu_seconds);
int sandbox_apply_fs(const char **allowed_paths, int count);
int sandbox_enter_darwin(struct sandbox_config *cfg);
```

#### 6.9.9 ReAct Function Calling

ReAct 循环使用 LLM 原生 Function Calling（而非文本解析 `Action: tool_name(args)` 格式）。LLM 返回 `tool_calls` 数组，每个元素包含 `function.name`、`function.arguments`（JSON）和 `id`。运行时将 `id` 回填到 `tool_call_id`，用于后续 `tool_role` 消息关联。

**优势**：
- 无需正则解析 LLM 自由文本，避免格式错误
- 原生支持多工具并行调用（LLM 可一次返回多个 `tool_calls`）
- 参数自动 JSON 结构化，减少解析歧义

**并行执行**：当 LLM 返回多个 `tool_calls` 时，每个工具调用在独立 pthread 中执行，通过 `async_tool_call` 结构跟踪完成状态，完成后逐一回灌 Observation。

#### 6.9.10 Guardrail 输出验证

当 LLM 返回不含工具调用的文本响应（即 `tool_call_count == 0`）时，进入 Guardrail 验证。Guardrail 用**客观可验证条件**替代 LLM 主观自评，确保输出质量：

| 验证条件 | 说明 |
|----------|------|
| `min_tool_calls` | 若期望有生成产物，检查本轮是否至少调用了 N 个工具 |
| `must_have_output` | 检查回答中是否包含生成产物标记（`saved:`/`.png`/`.mp4` 等） |
| `max_empty_rounds` | 连续空回答次数上限 |

**验证通过** → 进入 Final。
**验证失败** → 构造包含具体失败原因的 user message 回灌给 LLM，回到 THINKING 重试（最多 `guardrail_max_retries` 次）。

**与旧 Reflection 的区别**：

| 维度 | Reflection（旧） | Guardrail（新） |
|------|-----------------|-----------------|
| 评估方式 | LLM 自评（`VERDICT: SATISFACTORY`） | 客观条件验证 |
| LLM 调用 | +1 次/轮独立调用 | 0 次额外调用 |
| 解析方式 | `strstr`/`strncmp` 文本匹配 | 结构化条件判断 |
| 可靠性 | LLM 倾向给自己好评 | 基于可验证信号 |

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
struct skill_frontmatter {
	char name[64];
	char description[256];
	char license[64];
	char compatibility[64];
	char allowed_tools[256];
	struct { char key[64]; char value[128]; } metadata[8];
	int metadata_count;
};

struct skill_entry {
	struct skill_frontmatter fm;
	char skill_dir[512];		/* Skill 所在目录 */
	char *instructions;		/* SKILL.md 内容（frontmatter 之后） */
	int enabled;
	int activated;
};

struct skill_registry {
	struct skill_entry entries[32];
	int count;
};

void skill_registry_init(struct skill_registry *reg);
int skill_discover(struct skill_registry *reg, const char *dir);
struct skill_entry *skill_lookup(struct skill_registry *reg, const char *name);
int skill_activate(struct skill_entry *entry);
void skill_deactivate(struct skill_entry *entry);
char *skill_build_activated_instructions(struct skill_registry *reg);
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
	pthread_t thread;
	pthread_mutex_t mutex;
	volatile sig_atomic_t *cancel_flag;  /* 指向 react_sigint_flag */
};

void spin_init(struct spin_context *ctx, FILE *output);
void spin_start(struct spin_context *ctx, enum spin_state state, const char *message);
void spin_update(struct spin_context *ctx, const char *message);
void spin_set_sub(struct spin_context *ctx, const char *submessage);
void spin_stop(struct spin_context *ctx, enum spin_state final_state, const char *message);
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

- Tab 缩进（8 字符宽）；软上限 80，硬上限 100
- 函数名 `snake_case`，类型 `struct foo`；宏全大写
- 错误码使用负 `errno`（`-EINVAL` / `-ENOMEM`）
- 集中清理 `goto out;`
- C 风格注释，禁止 `//`
- `sizeof(var)` 而非 `sizeof(type)`
- 多语句宏 `do { } while (0)` 包裹
- 内存：统一通过 `xmalloc/xfree` 包装层调用，失败立即返回
- 静态检查：`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`，CI 必须 0 warning

---

## 8. 日志与可观测性

| 项 | 设计 |
|------|------|
| 日志级别 | debug / info / warn / error，运行时可调 |
| 输出目的地 | stderr（交互期 warn+）+ 文件（全量，5MB rotate × 3） |
| ReAct 轨迹 | 写入 `react_traces` 表 + `--trace-file` 可导出 JSON |
| Token 用量 | 每次 LLM 调用记录 `prompt_tokens / completion_tokens / cost_estimate` |
| 工具调用埋点 | 工具名、耗时、成功/失败、参数摘要（脱敏） |
| 错误上报 | 默认仅本地；可配置 `[telemetry]` 开关（opt-in） |
| 调试模式 | `MORPH_DEBUG=1` 打印每次 HTTP request/response |

---

## 9. 测试策略

| 层级 | 工具 | 覆盖目标 |
|------|------|---------|
| 单元 | GoogleTest（CMake FetchContent） | 核心模块 ≥ 70% |
| 集成 | Mock LLM（本地 HTTP server，返回固定 SSE 流） | ReAct 全流程 |
| 模糊 | libFuzzer / AFL++ | manifest.toml 解析、JSON 解析 |
| 内存 | Valgrind + ASan + UBSan | 0 lost，0 UB |
| 沙箱逃逸 | 专项用例集（见下） | 100% 阻断 |
| 跨平台 | GitHub Actions：ubuntu-22.04，macos-14 | 冒烟全过 |

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

| 里程碑 | 周次 | 交付物 | 验收 KPI |
|--------|------|--------|---------|
| **M1 / MVP** | W1–W4 | 项目骨架 + CLI + 文字对话（流式）+ 会话持久化 + Token 计数 + 滑动窗口 + 1 个 demo Ext（无沙箱） | 首 token < 1s；7 个 `/` commands 可用；Valgrind clean |
| M2 / V0.2 | W5–W7 | 文生图 + 图片理解 + 终端预览（kitty/sixel/iterm2） | 图片 P95 < 30s |
| M3 / V0.3 | W8–W10 | 文/图生视频 + mpv 播放 + 视频理解 + 异步轮询 | 视频任务可后台轮询 |
| M4 / V0.4 | W11–W13 | Ext 沙箱（seccomp+rlimit+landlock）+ install/enable/disable | 沙箱逃逸 6/6 阻断 |
| M5 / V0.5 | W14–W15 | 跨模态联动模板 + 摘要压缩 + 关键信息提取 | 长对话 token 节省 ≥ 40% |
| M6 / V1.0 | W16–W18 | 多模型切换 + Ext 市场（git）+ macOS sandbox-exec + Homebrew formula | macOS 冒烟通过 |

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
| Token 计数误差 | 截断/浪费 | 多 tokenizer + 保守估算 + 分批 | M1 |
| 静态体积超 8MB | 不达 KPI | 动态链接 + LTO + strip + 删除未使用代码 | M1 |
| Ext IPC 性能 | 流式卡顿 | 行缓冲 + 共享内存（P2） | M4 |

---

## 13. 开放问题

| # | 问题 | 状态 | 决策建议 |
|---|------|------|---------|
| 1 | 产品命名 | Open | M2 前定 |
| 2 | 优先接入哪些 API | **Decided**（M1）：OpenAI + 可灵 | — |
| 3 | 是否支持本地模型（Ollama） | Open | P2 评估 |
| 4 | SQLite 是否硬依赖 | **Decided**：是 | 简化数据层 |
| 5 | 终端图片是否 fallback feh/sxiv | **Decided**：否，只回退路径 | 减少依赖 |
| 6 | 视频理解是否本地抽帧 | Open | M3 评测精度 vs 成本 |
| 7 | 是否发布 Homebrew/AUR | M6 决策 | — |
| 8 | Ext 是否需 NetNS 隔离 | **Decided**：P2 | 默认 seccomp 即可 |
| 9 | `.so` Ext 是否需符号可见性限制 | **Decided**：是 | 仅导出 `ext_run` |