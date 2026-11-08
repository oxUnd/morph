# morph 需求文档

> **文档版本**：v0.7
> **完成状态**：**已完成（与当前实现对齐）**
> **实现基线**：v0.4.2，提交 `1d7d8ac`，更新日期 2026-11-08。
> **范围**：本文仅记录仓库中已经实现的功能、接口职责和资源边界。完成状态指本次文档同步完成；具体接口以对应头文件和配置 schema 为准。

## 0. 术语与缩写

| 术语 | 定义 |
| --- | --- |
| Agent | 封装 ReAct 循环、模型、工具和会话的运行实体 |
| ReAct | 模型推理、结构化工具调用、工具结果回填和最终回答的执行循环 |
| Runtime | 统一拥有模型、工具、权限、会话和 worker 生命周期的进程级运行时 |
| Tool | 内置工具、动态工具、MCP 工具和 Ext 的统一执行入口 |
| Ext | 通过 manifest 描述的原生共享库或可执行扩展 |
| Dynamic Tool | 使用内嵌 QuickJS 执行的会话级或持久 JavaScript 工具 |
| Session | 持久化到 SQLite 的会话、显示消息及模型历史 |
| Context Window | 一次模型请求允许容纳的输入和输出 token 预算 |
| MCP | 远程工具、资源及提示词的发现和调用协议 |
| Sub-agent | 使用独立提示、工具限制和执行上下文的子代理 |
| Memory | 显式偏好、记忆笔记、工作记录和有来源的长期摘要 |
| Task | 持久化的一次性或周期性任务，执行结果进入通知 inbox |
| Checkpoint | primary turn 的会话快照和 `apply_patch` 文件修改快照 |

## 1. 产品概述

morph 是以终端为主要入口的 AI Agent，核心使用 C11 实现，通过模型 API 完成文字对话、图片理解、图片生成和视频生成，并使用文件、进程、技能及子代理工具处理任务。

CLI 支持交互、单次执行和管道使用；可选 FastCGI 前端提供 HTTP API 与 SSE。各前端共同使用 runtime，不分别维护模型、数据库或 ReAct 生命周期。

会话、模型历史、记忆、任务、权限授权和生成产物元数据保存在本地。模型调用按所选 API 传输请求内容，凭据通过配置指定的环境变量等已支持入口解析。

## 2. 使用场景

| 场景 | 已实现的能力组合 |
| --- | --- |
| 内容创作 | 文字生成、改写、场景角色提示、图片和视频生成 |
| 图像处理 | 图片问答、参考图生成、局部编辑、跨图融合、标注、缩放及格式转换 |
| 项目任务 | 文件读取、目录分页、补丁编辑、受权限控制的命令执行与进程会话 |
| 多步骤任务 | Plan、ReAct、同步或异步子代理、并行扇出及结果合并 |
| 远程工具集成 | MCP stdio / Streamable HTTP、工具/资源/提示词发现 |
| 定时执行 | 一次性和周期任务、后台 Agent 执行、通知 inbox |
| Web 集成 | 可选 FastCGI API、认证、SSE、审批和问答 action |

## 3. 实现验收口径

| 项目 | 当前验收方式 |
| --- | --- |
| 功能行为 | GoogleTest、Mock LLM、本地 HTTP/SSE fixture 和 CLI/FastCGI 集成测试 |
| 交互行为 | PTY 测试验证选择、长文本、粘贴、取消、命令呈现及 composer 恢复 |
| 内存与边界 | `ENABLE_ASAN=ON` 同时启用 AddressSanitizer 和 UndefinedBehaviorSanitizer |
| 接口与所有权 | typed cleanup、借用/复制/转移约定，以及对应回归测试 |
| 源码约束 | `source_control_flow_conventions`、`cli_application_layout_conventions`；可选全局状态审计 |
| 平台相关能力 | 沙箱、终端、剪贴板及 MCP live 测试按实际环境运行，缺少条件时明确跳过 |

## 4. 已实现功能

### 4.1 范围

下列各节描述当前代码行为。资源预算、供应商限制和展示裁剪分别说明，不将有限预览等同于完整模型输入或持久化数据。

### 4.2 对话与会话

- 多轮文字对话、流式 reasoning/content 输出、取消和失败反馈。
- 创建、恢复、切换、重命名、删除会话，以及导出和分享会话内容。
- 显示消息与持久模型历史分别保存；支持检查有效上下文、原始记录、最近请求、诊断及修复。
- `/model` 查看或修改当前已配置文本后端的模型 ID。
- `/recap` 手动回顾；启用 `general.auto_recap` 后支持离开一段时间再返回的自动回顾。
- 用户输入支持 UTF-8；界面错误、状态和模型指令使用项目当前的英文文案。

### 4.3 上下文预算与压缩

- 使用 CL100K/O200K BPE 和 Unicode-aware 估算；按所选 adapter 的实际请求序列化内容估计输入。
- 输入上限取配置窗口与模型窗口的有效较小值，扣除**完整输出预算**和协议预留；发送前再次校验。
- `summarize_threshold_ratio` 默认 0.8，`compress_target_ratio` 默认 0.5；压缩目标不得超过实际可用输入预算。
- 保留近期历史、压缩早期内容，并保持工具调用及结果的配对关系。
- 持久模型历史支持重复压缩、既有摘要再次参与压缩和摘要来源链记录。
- 工具结果按 token 预算生成有效 JSON 和明确省略标记；原始结果不会先被固定字节前缀截断。
- `/context` 查看用量，`/compress` 手动触发压缩，`/history` 检查上下文成员及摘要来源。

完整边界见 [模型能力与资源限制](model-capability-limits.md) 和 [上下文与进程恢复](context-and-process-recovery.md)。

### 4.4 文字与提示模板

- 文字生成、改写、摘要和 Markdown/纯文本等由所配置文本模型完成。
- 支持系统提示文件、目录和模板渲染。
- 已有 general、developer、photographer、photo_editor、writer、designer、video_creator、researcher 场景角色模板。
- Skill 目录先进入可用能力清单；激活后加载完整指令。

实现位于 [提示模板](../src/prompt/template.c) 和 [场景角色模板](../prompts/scenarios/)。

### 4.5 图片生成、理解与处理

| 能力 | 当前行为 |
| --- | --- |
| 图片生成 | `img_gen` 使用独立图像模型；尺寸和参考图能力由配置的 adapter 校验 |
| 参考图 | 单次输入最多 10 张参考图；具体格式和尺寸按 adapter 处理 |
| 图片理解 | `img_qa` 使用独立 `[model.vision]`；支持 `prompt`、`max_tokens`、`max_dim`、`detail`、`timeout_seconds` |
| 理解精度 | 默认高细节、最大边 2048；`detail=original` 在共享图像安全边界内保留原尺寸 |
| 理解输出 | 默认继承视觉模型输出 token 预算 |
| 局部编辑 | `img_inpaint` 使用 bbox+label 标注生成区域编辑请求 |
| 跨图融合 | `img_compose` 使用 arrow+label 标注及本地预合成生成融合请求 |
| 本地处理 | `img_info`、`img_annotate`、`img_resize`、`img_convert` |
| 展示 | 终端图像协议与文件路径回退；具体可用方式取决于终端 |

`img_inpaint` 和 `img_compose` 使用 `img_annotate` 生成的标注 JSON。理解模型、图像生成模型和文本模型分别配置。

### 4.6 视频生成与播放

- `vid_gen` 使用独立视频模型，支持文字及参考图片输入。
- 支持按模型能力校验的 `reference_videos`、`reference_audios` 和 `generate_audio`；Seedance 1.5 及以上支持生成音频，2.0 及以上支持多参考图片、参考视频和音频。
- 使用 Volcengine 视频 adapter 提交任务、轮询状态、下载结果并记录产物。
- 视频路径可交给终端渲染入口或 `mpv` 播放；失败时保留路径反馈。
- `duration` 转发供应商，支持范围取决于所选模型和 API；参考素材与音频组合由 adapter 能力校验。

### 4.7 ReAct 推理协同

ReAct 使用结构化 Function Calling，执行 Thought → Action → Observation → Guardrail → Final。文本生成由 LLM 直接完成；文件、媒体、进程和远程能力通过工具调用执行。

模型返回多个工具调用时，运行时可以并行执行并统一回收结果。达到迭代上限、取消、检查失败和模型错误保留明确 outcome，不将异常结束当成正常完成。

### 4.7.1 统一事件与 Hooks

- startup、ReAct、工具、MCP、审批、产物和后台任务使用统一 dotted event 名称及结构化 payload。
- CLI 和 FastCGI 分别渲染同一运行时事件语义。
- Hooks 支持配置加载、信任、启停、匹配、超时、取消、权限决定及上下文补充。
- 大段 Hook 上下文保存到私有文件并附恢复路径；保存失败会阻断处理，不能丢失 deny/ask 决定后继续执行。

参见 [事件系统](event-system.md) 与 [Hooks](hooks.md)。

### 4.8 Ext 扩展

| 类型或操作 | 当前行为 |
| --- | --- |
| 自动发现 | 扫描配置目录，解析 `manifest.toml`，注册工具或 Guardrail 规则 |
| exec 扩展 | 子进程执行，通过 stdin/stdout JSON-RPC 交换请求与最终结果 |
| 原生共享库 | `dlopen`/`dlsym` 后在宿主进程直接调用，适用于可信扩展 |
| GitHub 安装 | `/ext install` 支持 GitHub 来源、引用及子目录选择 |
| 工具管理 | `/ext list`、`/ext info` 查看已注册工具及元数据 |

exec 扩展进入配置的进程沙箱和资源限制；共享库扩展不具有子进程 OS 隔离。扩展 manifest 的有界字段超长、非法 UTF-8 和资源整数溢出会明确报错。

### 4.9 长期记忆与偏好

- SQLite 保存 facts、episodes、procedures、变更和显式 preferences。
- 显式偏好在接受输入时独立持久化，不依赖最终模型回答是否成功。
- 偏好支持 personal / project / session 作用域，有效值按 session > project > personal 覆盖。
- 每个 runtime 拥有独立异步 worker；任务输入深拷贝，并通过会话 generation 防止旧任务覆盖新状态。
- 上下文默认按 3000 Unicode 码点预算选择完整条目；数量或字符预算造成省略时显式标记，可通过 `memory` 分页恢复。存在显式偏好时保留至少 512 码点的上下文预算。
- 完整记忆仍保存在数据库，事实不会仅因超过 200 字节而被自动丢弃。
- `memory` 支持类型、作用域及 `offset_chars` / `max_chars` 分页；每页最多 4000 码点，并按最终完整 JSON envelope 的 token 预算缩小。
- `memory_preference` 管理显式偏好，`memory_update` 处理明确的记住、纠正、删除请求；CLI `/memory` 支持 show/set/unset/history/explain/jobs/retry 及清除记忆。多用户前端继续应用用户范围约束。

### 4.10 MCP

- stdio 与 Streamable HTTP 传输，JSON-RPC 请求/响应、超时、取消及连接清理。
- 发现并注册 tools、resources、prompts；支持启动后台发现和执行时按需连接。
- `/mcp` 管理服务器、连接及发现内容。
- 当前协议常量为 `2026-07-28`，兼容回退版本为 `2025-11-25`；具体握手策略由客户端实现决定。
- 工具描述和 schema 使用动态所有权；stdio 接收帧/缓冲上限 16 MiB，超限报错并断开连接。

接口见 [mcp.h](../src/mcp/mcp.h)，后台发布见 [runtime MCP](../src/runtime/mcp_background.c)。

### 4.11 子代理

- 配置独立系统提示、模型 ID、工具限制、上下文策略和输出 schema。
- `delegate(agent, task)` 异步委派，`agent_status(task_id)` 查询，`fanout(agent, tasks, merge)` 并行扇出。
- 配置的子代理可注册同步工具 `agent_<name>(task)`。
- 上下文策略支持 full / summary / task_only；合并策略支持 synthesize / concat / raw。
- 最多 8 个同时活跃的任务；完成或失败释放执行槽，稳定任务记录和结果继续保留。
- `fanout` 每批最多 8 项；超限或可用槽不足时在启动前明确拒绝。嵌套深度最多 2。
- 后代使用共享权限 authority，保留独立任务记录、事件和会话关联。

### 4.12 Guardrail

- 支持 C 函数、LLM 评估以及 exec / 共享库插件规则。
- 检查点为 input、tool_output、output；配置控制规则、重试和检查器错误策略。
- 检查未通过与检查器自身失败分别记录；`on_error` 决定继续或阻断。
- LLM 检查提示动态构造，完整保留检查内容和响应要求。
- Guardrail 负责结果检查；工具访问与审批由统一权限层处理。

### 4.13 FastCGI

- `BUILD_FASTCGI=ON` 构建可选 HTTP API 前端。
- 提供会话、turn、canvas 和 action 接口，使用 SSE 推送运行时事件。
- 支持 Bearer 凭据及配置的受信任反向代理身份入口。
- approve/reject/cancel 和问答 action 进入共享运行时交互契约。
- 请求体超过 4 MiB 返回 HTTP 413；问答选项超限明确报错。

### 4.14 动态 JavaScript 工具

- `morph-js-runner` 内嵌 QuickJS，执行 `run(args)` 或 `async run(args)`。
- `tool_create`、`tool_promote`、`tool_delete` 管理会话/持久工具；`tool_history`、`tool_diff`、`tool_rollback` 管理版本。
- 工具描述、input/output schema 完整保存，源代码默认最多 256 KiB，可配置。
- Host API 提供受能力、路径、命令和网络配置约束的 fs、image、canvas、fetch、env、exec、WASM 等能力。
- Host fetch、exec 子调用及 runner stdout 使用配置的捕获预算，默认 1 MiB；超限明确报错，不解析部分结果为成功。
- runner 顶层 stderr 作为有界诊断预览，超限附省略标记并记录诊断。
- fetch 支持校验后的请求头，包括服务器支持时的 Range；拒绝头注入及非法 NUL 输入。
- local/server 配置分别控制默认 capabilities 与允许范围。

接口及运行语义见 [QuickJS 工具](quickjs-tools.md)。

### 4.15 定时任务与通知

- `/tasks add <unix_time|+seconds> <prompt>` 创建一次性任务。
- `/tasks every <seconds> <prompt>` 创建周期任务；支持 list/show/update/cancel/run。
- 任务和通知持久化，执行具有 claim、重试、取消和结果提交检查。
- 到期 Agent 任务通过 runtime 进入 ReAct；结果写入通知 inbox。
- CLI 动态保存完整任务 prompt 和命令参数；展示标题的缩短不改变执行内容。

参见 [定时任务](scheduled-tasks.md)。

### 4.16 模型能力与资源限制治理

| 项目 | 已实现行为 |
| --- | --- |
| 调用身份 | 工具名称、调用 ID、本地关联 ID、provider item ID 使用动态字符串，执行、克隆、重放及后续请求保留完整值 |
| 凭据 | 环境变量长凭据使用完整 owned 存储，模型 clone 独立持有，Authorization 动态构造 |
| 响应终态 | Chat/DeepSeek 和 Responses 校验流终态；提前 EOF、失败及非长度原因 incomplete 返回错误 |
| 输出耗尽 | 保留可用的部分文本并报告长度限制，不执行未完成工具调用 |
| 审批输入 | 独立模型 reviewer 接收完整目标并继承配置预算；完整请求无法容纳时按失败路径 defer |
| 工具结果 | 历史预算处理保持 JSON 有效并标明省略，保留工具自身的分页或文件恢复入口 |
| 进程输出 | 有界内存窗口与独立脱敏 spool；元数据说明已捕获范围和无法恢复的缺口 |
| 长输入 | CLI 参数动态保存；问答和自定义答案使用明确预算，终端编辑跨容量扩容保留 UTF-8 与 NUL 边界 |
| 长片段计数 | BPE 使用索引最小堆保持 rank / 最左优先语义，避免长连续文本的反复全量扫描 |
| 词表读取 | 动态读取和有界解码，支持最多 65535 字节的 token，超出表示范围拒绝加载 |
| 文件修改 | checkpoint 在写入前验证 postimage 及总预算；seal 和 restore 再核验真实数据 |

资源上限及超限行为汇总见 §10 和 [模型能力与资源限制](model-capability-limits.md)。

## 5. CLI 交互

### 5.1 启动与执行

```sh
cmake -S . -B build
cmake --build build
./build/bin/morph --help
./build/bin/morph
```

交互模式使用 TUI composer、footer 和历史输出区域；用户提交内容使用 `›` 提示。CLI 根 Application 统一拥有交互、plain 和单次执行模式。

### 5.2 输入与问答

- 直接输入文字，或结合本地图片路径提交任务；`/image <path>` 提供图片入口。
- `/` 命令作为本地动作处理，不转成模型 steering 文本。
- `ask_user` 支持单选、多选、自定义答案、推荐项以及 required/optional 响应策略。
- 最多 64 个选项，答案文本合计最多 1 MiB；超限明确报错，包括 optional 超时推荐项回退。
- required 问题不自动作答；optional 支持无输入超时处理，编辑活动会暂停自动回退。
- 选择和超时问答使用共享选择组件；无 Readline 的交互自由文本也使用共享编辑器，Readline 模式的自由文本沿用 Readline。选择组件的长粘贴合并重绘。
- plain 模式在后台执行 turn，由 UI owner 持续处理事件和问答调用。

### 5.3 输出与本地命令呈现

- reasoning、回答和工具事件使用各自的输出块；Working、耗时和取消提示由 UI owner 渲染。
- 工具预览的显示裁剪与模型结果、持久化内容分别处理；完整详情由详情视图查看。
- Markdown 流式预览与已提交内容共享稳定边界，媒体由渲染回调处理。
- 已提交本地命令保留为普通背景上的独立紫色行，结果与成功回执不使用用户消息背景。
- `/hooks`、`/resume`、`/history` 面板取消后保留命令行并恢复一个 composer，不打印关闭回执。
- 未识别命令明确报错并清空编辑输入；已知但暂不能执行的命令保留重试输入。
- 命令与首个结果之间、已完成结果组之后各有约定的空行；Readline 回忆历史独立保存。

参见 [CLI 工具输出](cli-tool-feed.md)、[CLI 应用布局](tui-application-layout.md) 和 [TUI 盒模型](tui-box-model.md)。

### 5.4 已实现命令

命令参数及别名以 `/help` 和 [commands](../src/sapi/cli/commands/) 注册表为准。

| 类别 | 命令 | 功能 |
| --- | --- | --- |
| 基础 | `/help`、`/quit`、`/clear` | 帮助、退出、清屏 |
| 模型与上下文 | `/model`、`/context`、`/compress`、`/recap` | 模型 ID、预算、压缩和会话回顾 |
| 结果与配置 | `/trace`、`/credits`、`/config`、`/copy` | 轨迹、积分、配置、复制最后回答 |
| 会话 | `/new`、`/resume`、`/switch`、`/rename`、`/delete` | 会话生命周期 |
| 历史 | `/history` | 有效上下文、显示消息、模型历史、最近请求、条目、诊断、修复和导出 |
| 导出与分享 | `/save`、`/share` | 导出会话、生成分享内容 |
| 回退 | `/rewind` | 恢复代码、对话或两者 |
| 媒体 | `/image`、`/render` | 图片输入和文件渲染 |
| 工具与扩展 | `/ext list`、`/ext info`、`/ext install` | 工具列表、详情及 GitHub 扩展安装 |
| 技能交互 | `/skill` | 列表、详情、激活和停用 |
| 技能安装管理 | `morph skills add/list/remove/update` | 顶层 CLI 命令，管理本地或 Git 来源的技能 |
| MCP | `/mcp` | 服务器与发现内容管理 |
| 权限 | `/permissions` | access/approval/reviewer 及持久 grant 管理 |
| Hooks | `/hooks` | Hook 信任、状态及管理 |
| 记忆 | `/memory` | 查看、设置/取消偏好、历史、解释、后台任务及清除记忆 |
| 任务与通知 | `/tasks`、`/inbox` | 定时任务和通知管理 |
| 进程 | `/jobs` | 检查及管理受管进程任务 |
| 同步 | `/sync` | 当前实现的同步配置及会话同步操作 |

## 6. 技术架构

### 6.1 分层与生命周期

```text
CLI Application / FastCGI
          |
          v
runtime_open / runtime_execute_turn / runtime_close
          |
          +-- ReAct / history / compression / memory / plan / sub-agents
          +-- tool registry / permissions / approvals / hooks
          +-- models / MCP / Ext / QuickJS / managed processes
          +-- sessions / persistence / tasks / sync / credits / events
```

前端持有 opaque `struct runtime *`，通过公开 DTO、快照和服务接口交互。runtime 创建完整依赖图，在失败或关闭时先停止 worker、回收借用，再按依赖逆序释放资源。

每个 turn 使用统一 request、result 和临时绑定作用域；标题、回顾和模型审批使用隔离模型请求，避免共享可变请求状态。接口与源文件归属见 [Runtime 架构](runtime-architecture.md)。

### 6.2 ReAct 执行

#### 6.2.1 状态与终止

模型请求、工具执行、Observation、Guardrail 和 Final 有明确阶段。取消、模型错误、检查失败、达到迭代上限和正常完成分别记录。

工具的默认墙钟预算为 `react.tool_timeout_seconds`，工具配置或参数可覆盖。超时/取消后仍需要回收 worker；释放工具运行时、模型或数据库之前完成必要的 drain。

#### 6.2.2 工具与错误

工具通过结构化结果回填成功、业务错误及 artifacts；进程内 worker 与子进程分别遵循其取消和回收契约。有限异步进程任务在最终回答前收集，`notify_only` 和 `service` 按明确声明处理。

#### 6.2.3 提示准备

稳定系统提示包含 core/default 或自定义行为及完整技能目录。运行环境和项目指令作为前置请求消息；已激活技能、计划/进程/产物状态、记忆和 Hook 上下文在请求末端动态生成，不反复持久化。旧式纯文本请求路径合并等价文本。

项目指令受 `context.project_doc_max_bytes` 控制；不能完整纳入的文件保留路径及 `file_read` 恢复要求。

#### 6.2.4 流式结果

文本和 reasoning 通过结构化事件流输出。工具名称、ID、arguments 及 Responses item identity 在增量事件与完成事件之间正确关联；完成事件可补全最终参数。缺失终态或不完整调用不转为成功工具执行。

### 6.3 历史与压缩

显示消息、canonical 模型历史、请求快照、压缩记录和来源关系分别持久化。有效重放窗口保持完整工具调用组，并在发送前校验真实组装后的请求预算。

压缩先计算固定请求开销及可用历史预算，再生成和提交候选摘要；提交时检查历史是否仍有效。重复压缩可包含既有摘要，并继续记录来源关系。原始记录和可见上下文不混为同一存储视图。

相关实现见 [history.h](../src/agent/history.h)、[history_compaction.c](../src/agent/history_compaction.c) 和 [react_request.c](../src/agent/react_request.c)。

### 6.4 构建与技术选型

| 部分 | 当前实现 |
| --- | --- |
| 语言与构建 | C11 / C++17 测试，CMake ≥ 3.20，要求 GCC/Clang cleanup attribute |
| 网络与存储 | libcurl、SQLite3、pthreads |
| TUI | 自有 Application/View/Canvas/widgets，libuv；Readline 可选 |
| JSON / TOML / Unicode | vendored cJSON、tomlc17、utf8proc，经项目适配接口调用 |
| Markdown | `fronts/morph-markdown` 及 Kitty SDK；cmark-gfm 由该构建集成 |
| 图像 | stb_image / write / resize2；动态工具图像与画布使用 libvips、cairo、pangocairo |
| 数学排版 | MorphMarkdown 构建中的 MathJax-C 及字体/排版依赖 |
| JS / WASM | 内嵌 QuickJS、wasm3 |
| Shell 解析 | tree-sitter / tree-sitter-bash |
| Tokenizer / 哈希 | BPE 词表、Unicode-aware 估算、BLAKE3 等项目构建依赖 |
| 沙箱 | Linux seccomp denylist / Landlock / rlimits；macOS Seatbelt |
| 视频播放 | 外部 `mpv` 可执行文件 |
| FastCGI | 可选 libfcgi 前端 |
| 测试 | GoogleTest、Python/PTY 集成及 ASan/UBSan 构建开关 |

具体依赖、版本和条件以 [根 CMakeLists.txt](../CMakeLists.txt) 及各模块构建文件为准。

### 6.5 模型与 adapter

| 角色 | 已实现 adapter |
| --- | --- |
| 结构化文本/工具请求 | `openai-chat-compatible`、`openai-responses`、`deepseek` |
| 图像生成 | `openai-images`、`volcengine-images` |
| 视频生成 | `volcengine-videos` |

角色分别读取 `[model.text]`、`[model.vision]`、`[model.image]`、`[model.video]`；可选 `[model.approval]` 指定独立 reviewer。provider、API 地址、模型 ID、adapter、凭据、额外请求字段和输出预算按各角色配置。

adapter 决定请求序列化、响应解析和能力校验；供应商返回的 usage 与本地输入估计分别保存。图像理解需要视觉模型配置，图像生成配置不会替代视觉模型。

`img_qa` 和普通 chat 辅助调用当前使用 Chat Completions 兼容接口；模型审批调用结构化文本请求路径。

### 6.6 数据持久化

| 数据 | 实现中的主要存储 |
| --- | --- |
| 会话与显示消息 | `sessions`、`messages`、`message_attachments`、`react_traces` |
| 模型历史与压缩 | `model_history_items`、`history_compactions`、`history_summary_sources`、`model_request_snapshots`、`history_compaction_attempts` |
| 记忆与偏好 | profiles/facts/episodes/procedures、scopes/preferences/candidates/jobs |
| 子代理 | `sub_agent_tasks`、`sub_agent_events` |
| 计划与权限 | `session_plans`、`permission_grants` |
| 定时任务与通知 | `scheduled_tasks`、`notifications` |
| 产物与计费 | `outputs`、`credit_events` |
| 文件与对话回退 | `code_checkpoints`、`code_checkpoint_files` 及 checkpoint 管理的会话快照 |

数据库开启 WAL，schema/migration 由 [database.c](../src/db/database.c) 和对应持久层维护。产物记录包含实际请求、provider、工具/调用/turn 标识、recipe、媒体尺寸及字节信息。checkpoint 的捕获、seal、冲突检查和恢复由 [checkpoint_store.c](../src/persistence/checkpoint_store.c) 实现。

### 6.7 配置

默认读取 `~/.morph/config.toml`，可用 `-c` / `--config` 指定文件。以下是已支持字段的节选，不替代 [完整示例](../config.toml.example) 或 [配置 schema](../src/config/schema.c)。

```toml
[model.text]
provider = "openai"
adapter = "openai-chat-compatible"
model = "gpt-4o"
api_base = "https://api.openai.com/v1"
api_key_env = "OPENAI_API_KEY"
context_limit = 128000
max_tokens = 16384
timeout_seconds = 600
retry_count = 3

[model.vision]
provider = "openai"
model = "gpt-4o"
api_base = "https://api.openai.com/v1"
api_key_env = "OPENAI_API_KEY"
max_tokens = 16384

[permissions]
access = "workspace"
approval = "on_request"
reviewer = "user"

[context]
summarize_threshold_ratio = 0.8
compress_target_ratio = 0.5
keep_recent_rounds = 6
in_turn_compaction = true
protocol_reserve_tokens = 4096
tool_result_max_tokens = 8000
project_doc_max_bytes = 32768

[exec]
shell = "/bin/bash"
default_timeout_ms = 120000
yield_time_ms = 10000
max_wait_ms = 600000
max_inline_output = 32768
max_session_output = 1048576
kill_grace_ms = 500

[memory]
enabled = true
max_facts = 6
max_past_tasks = 4
max_tips = 4
max_reply_chars = 3000

[dynamic_tools]
enabled = true
runtime = "quickjs"
mode = "local"
max_source_bytes = 262144
default_max_output_bytes = 1048576
```

- `max_tokens` 是一次响应的完整生成预算，thinking 和可见回答共同使用；省略时 fallback 为 4096，不代表示例模型的最大能力。
- `[model.text]` 还支持 `extra_body_json` 等字段；请求托管字段不能被额外 JSON 覆盖。
- 权限策略由 `[permissions]` 统一控制；`react.permission_profiles` 为启动时路径授权补充来源。
- `process` 默认等待 30000ms，模型可在配置的 `max_wait_ms` 范围内指定等待时间。
- 执行日志、模型凭据和本地数据的存储行为以配置及对应 owner 为准，敏感值在模型历史和进程输出恢复路径中脱敏。

### 6.8 源码归属

| 路径 | 职责 |
| --- | --- |
| `src/sapi/cli/main.c` | CLI 入口 |
| `src/sapi/cli/` | application、pages、widgets、models、services、commands、presentation、adapters |
| `src/sapi/fastcgi/` | 可选 FastCGI 前端 |
| `src/runtime/` | 生命周期、请求、会话、权限、Hooks、任务、同步及运行时服务 |
| `src/agent/` | ReAct、工具系统、历史、压缩、Tokenizer、记忆、Plan、子代理 |
| `src/models/` | 文本、视觉、图像和视频 adapter |
| `src/permission/` | 策略、操作、grant 及 reviewer 路由 |
| `src/exec/` | 受管进程会话和输出恢复 |
| `src/persistence/`、`src/db/` | typed 存储、schema、事务及迁移 |
| `src/session.c` 等 session 模块 | 会话和模型历史持久化接口 |
| `src/tui/` | core、terminal、render、widgets 和样例 |
| `src/skill/`、`src/prompt/` | Skill 发现/激活和提示模板 |
| `src/mcp/`、`src/ext/`、`src/ipc/` | 远程能力及扩展调用 |
| `src/js_runner/`、`src/sandbox/` | QuickJS Host API 与平台沙箱 |
| `src/event/`、`src/credits.c`、`src/credits.h`、`src/sync/`、`src/util/` | 事件、计费、同步和基础组件 |
| `prompts/`、`skills/`、`exts/` | 已提供的提示、技能及扩展示例 |
| `tests/` | 单元和集成测试 |

公共入口、构建目标和 source manifest 以当前头文件、`CMakeLists.txt`、`sources.list` 为准。

### 6.9 关键接口契约

#### 6.9.0 所有权与作用域清理

普通堆对象使用单一明确 owner，局部拥有者初始化为 `NULL` 或 `{0}`，使用 `MORPH_CLEANUP(fn)` 和 typed cleanup。允许部分初始化，销毁前先回收依赖它的异步任务。

`*_borrow` 返回有生命周期限制的视图，`*_copy` / `*_clone` 创建独立副本，fallible `*_take` 仅成功时清空来源，typed `*_steal` 执行无分配转移。禁止浅拷贝拥有指针的结构。参见 [状态所有权](state-ownership.md)。

#### 6.9.0a 基础容器

| 容器 | API 与约束 |
| --- | --- |
| [morph_buf](../src/util/buf.h) | 动态拼接字节/字符串；插入调用方使用 `reserve` 时须显式计入数据和结尾 NUL 所需空间 |
| [morph_array](../src/util/array.h) | `push_borrow` / `push_n_borrow` / `borrow` / `reserve`；增长后不得继续使用旧槽位指针 |
| [morph_strmap](../src/util/strmap.h) | `set_borrow` / `borrow` / `contains` / `remove`，保存字符串索引 |
| [morph_str](../src/util/str.h) | 非拥有字符串切片及明确的复制接口 |
| [morph_queue](../src/util/queue.h) | 侵入式队列，按 owner 生命周期保存稳定节点 |

业务代码复用这些容器和 [UTF-8 适配](../src/util/utf8.h)，不重复实现字符串增长、哈希表或 Unicode 解码。

#### 6.9.1 ReAct

[react.h](../src/agent/react.h) 定义执行上下文及生命周期；[react_output.h](../src/agent/react_output.h) 定义结构化输出。事件字段在回调期间借用。前端通过 runtime 执行 turn，不直接接管 ReAct 依赖。

#### 6.9.2 工具系统

接口分为 [tool_registry.h](../src/agent/tool_registry.h)、[tool_execution.h](../src/agent/tool_execution.h)、[tool_result.h](../src/agent/tool_result.h)。注册表动态增长，注册时复制描述和 schema；JSON 与文本输入由 `input_kind` 区分。

`tool_register_borrow` 借用 user data，`tool_register_take` 仅成功时转移拥有权。异步执行通过 retained handle 保持执行能力，不跨注册表结构变更持有裸 entry。结果具有结构化 envelope、artifacts、错误和显式清理契约。

#### 6.9.3 模型

[llm.h](../src/models/llm.h) 提供 chat、工具调用、reasoning/content 流、图像问答、clone、请求视图和输入估计接口。响应及 clone 结果拥有动态字段，输入视图只在请求期间借用；分别使用相应 cleanup 释放。

`model_llm_clone` 创建独立并发客户端，`model_request_view_init` 借用源资源，不作为独立模型销毁。图像和视频生成使用各自 adapter 接口。

#### 6.9.4 工具清单

| 类别 | 已注册工具或配置生成工具 |
| --- | --- |
| 查询与偏好 | `credits`、`memory`、`memory_preference`、`memory_update` |
| 文件与修改 | `file_read`、`file_list`、`file_info`、`apply_patch`、`checkpoint` |
| 图像 | `img_gen`、`img_qa`、`img_info`、`img_annotate`、`img_inpaint`、`img_compose`、`img_resize`、`img_convert` |
| 视频 | `vid_gen` |
| 进程与授权 | `exec`、`process`、`request_permissions` |
| 规划与交互 | `plan`、`ask_user`、`activate_skill`、`config_edit`、`tasks` |
| 动态工具管理 | `tool_create`、`tool_promote`、`tool_delete`、`tool_history`、`tool_diff`、`tool_rollback` |
| 子代理 | `delegate`、`fanout`、`agent_status`、配置生成的 `agent_<name>` |

工具是否对某个会话可见由配置、前端支持及注册状态决定。参数和输入格式以 [tools 源码](../src/agent/tools/) 中的 schema 为准。

`file_read` 使用 `file_path`，支持行/字节游标、版本校验和长行连续读取；`file_list` 使用 `dir_path` 和 `after`，返回 `next_after`。进程的 `poll` / `wait` 返回游标、文件路径和捕获范围元数据。

#### 6.9.5 压缩与恢复

[context.h](../src/agent/context.h)、[compress.h](../src/agent/compress.h) 定义底层消息和压缩操作；主执行路径通过持久模型历史管理重放和来源链。历史默认工具结果预算为 8000 tokens，包含结构化控制字段和省略元数据。

较大输出通过工具自身分页、进程 spool 或 artifact 路径恢复；压缩后的历史不作为完整输出副本。

#### 6.9.6 Ext 生命周期

[ext.h](../src/ext/ext.h) 和 [manifest.h](../src/ext/manifest.h) 定义加载、执行、取消和销毁接口。原生共享库直接调用；exec 类型使用受控子进程。共享库不能据其 manifest 推断已有 OS 进程隔离。

#### 6.9.7 Ext IPC

exec 扩展接收以换行结束的 JSON-RPC `run` 请求，返回最终 `result` 或 `error`，stderr 用于诊断。stdout 捕获上限 1 MiB，超限明确返回错误；大结果保存为文件后返回路径。通信失败或取消时终止并回收子进程。

接口见 [jsonrpc.h](../src/ipc/jsonrpc.h)。原生共享库不经过此 IPC。

#### 6.9.8 沙箱与权限

- Linux 使用默认 ALLOW 的 seccomp denylist 阻断逃逸相关 syscall，并结合 Landlock 路径访问和 rlimits。
- macOS 使用 Seatbelt / `sandbox_init` 的 SBPL 规则。
- 共享权限层按 access、approval、reviewer 判断操作，维护 turn/session/persistent grants。
- 写入授权与删除授权分别判断；已有批准范围由具体工具复用，后代共享 authority。
- 模型 reviewer 使用完整目标及独立请求；无法完成检查时按策略 defer，不以目标前缀替代完整审查。

参见 [权限设计](permissions-design.md)、[验收记录](permissions-acceptance.md) 和 [sandbox.h](../src/sandbox/sandbox.h)。

#### 6.9.9 Function Calling

Chat compatible、Responses 和 DeepSeek 使用各自 codec 编解码工具调用。文本 patch 由支持的输入格式传递；调用身份、provider item identity 和结果关联分别保存。多个工具的 worker 在释放共享依赖前 join 或 drain。

#### 6.9.10 Guardrail 接口

[guardrail.h](../src/agent/guardrail.h) 定义 hook、规则、pass/fail/error 结果及错误策略。配置、LLM 和 Ext 规则共用检查结果语义，检查器失败与检查不通过分别处理。

#### 6.9.11 记忆接口

[memory.h](../src/agent/memory.h) 定义偏好接收、上下文构建、同步/异步整合和 worker 生命周期。[memory_repository.h](../src/persistence/memory_repository.h) 与 [preferences.h](../src/persistence/preferences.h) 提供 typed 存储；显示摘要不能替代完整记录。

#### 6.9.12 MCP 接口

[mcp.h](../src/mcp/mcp.h) 定义客户端、描述符及请求接口；工具、资源和提示词描述符使用独立动态所有权。runtime 负责连接、后台发现、原子发布和清理，前端通过 MCP 服务 DTO 访问。

#### 6.9.13 子代理接口

[sub_agent.h](../src/agent/sub_agent.h) 提供同步、委派、fanout、状态及事件接口。任务历史采用稳定 owner，活跃容量与历史数量分离；失败和完成都归还执行槽。

#### 6.9.14 Tool Context

工具运行时携带当前权限、HTTP/取消、数据库、会话和宿主服务上下文。权限 context 保持 opaque，通过 [permission/context.h](../src/permission/context.h) 使用；具体工具描述操作，opaque 工具使用注册表权限 gate。

#### 6.9.15 Plan

[plan.h](../src/agent/plan.h) 提供多步骤计划及状态操作，`plan` 工具向模型开放；会话计划独立持久化。

#### 6.9.16 Tokenizer

[tokenizer.h](../src/agent/tokenizer.h) 和 [bpe.h](../src/util/bpe.h) 定义计数接口。分批只控制临时工作量，不截断剩余输入；长连续片段使用等价合并堆。分词失败或分配失败保留相应保守计数路径。

### 6.10 Skill

- 按项目 `.agents/skills`、配置或默认用户目录发现 Skill，同名采用先发现者。
- 使用默认 `~/.morph/skills` 时，还发现 `~/.agents/skills`。
- `SKILL.md` frontmatter 使用 `name`、`description`、`license`、`compatibility`、`allowed-tools`、`metadata` 等已解析字段。
- 名称最多 64、描述最多 1024 个 Unicode 码点；metadata 最多 16 项，frontmatter 预算 1 MiB。超长和非法 UTF-8 返回错误。
- 注册表动态增长并保持 entry 地址稳定；正文在激活时懒加载，每次请求临时注入。
- 顶层 `morph skills add/list/remove/update` 支持 Skill 安装及管理；`/skill` 提供会话内查看、激活和停用，模型通过 `activate_skill` 加载完整指令。

接口见 [skill.h](../src/skill/skill.h)、[skill_parse.c](../src/skill/skill_parse.c) 及 [CLI 管理](cli-management.md)。

### 6.11 TUI 与进度呈现

TUI 公开 Application、View、Canvas 和 widget 对象。自定义类型将公共 base 放在首字段，通过可选 method table 扩展；可变 UI 操作归 owner 线程，worker 通过消息提交状态。

View 使用四边 margin/padding：measure 返回含 margin 的外尺寸，arrange 接收 outer slot；size limits 含 padding、不含 margin，兄弟 margin 相加。

CLI Application 复用该框架管理模式和页面，feed 显示 Working、耗时、推理、回答、工具与后台任务。`util/spin` 是独立可复用工具，不作为 CLI 的全部线程和输出契约。

参见 [TUI 架构](tui-architecture.md)、[应用布局](tui-application-layout.md) 和 [盒模型](tui-box-model.md)。

### 6.12 C 编码规范

- Tab 缩进 8 列，软上限 80、硬上限 100；函数 snake_case，类型 `struct foo`，宏全大写。
- 项目自有 C/C++/Objective-C 源码禁止 `goto`；使用已初始化 owner、typed scope cleanup 和显式返回。
- 注释使用 `/* */`；错误、日志、UI 和模型指令字符串使用英文，非英文可作为输入或测试数据。
- `sizeof(var)`；系统路径使用 `PATH_MAX`，逻辑名称使用项目常量。
- 可变字符串、数组和索引使用共享容器；不得在增长后继续持有失效借用。
- fallible take 失败保留来源；每个 owner 有独立 cleanup/destroy；不引入全局变量。
- 多语句宏使用 `do { } while (0)`，错误使用负 errno 或 `MORPH_ERR_*`。
- 使用 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`；源码约束由相关 convention 检查执行。

## 7. 错误处理

`morph_err_t` 为 `int`，零成功、负值错误。系统错误在失败位置捕获 errno，使用 `morph_error_from_errno` 保留原始失败；错误信息通过 `morph_strerror` 提供。

| 错误域 | 已定义常量 |
| --- | --- |
| 配置与状态 | `MORPH_ERR_NOT_CONFIGURED`、`MORPH_ERR_NOT_INITIALIZED`、`MORPH_ERR_CONFIG` |
| API 与传输 | `MORPH_ERR_API`、`MORPH_ERR_NETWORK`、`MORPH_ERR_PROTOCOL`、`MORPH_ERR_LLM` |
| 数据与处理 | `MORPH_ERR_PARSE`、`MORPH_ERR_DB`、`MORPH_ERR_FORMAT`、`MORPH_ERR_PROCESSING`、`MORPH_ERR_LOAD` |
| 执行边界 | `MORPH_ERR_SANDBOX`、`MORPH_ERR_REACT_MAX_ITERATIONS`、`MORPH_ERR_CONTEXT_LIMIT` |

`MORPH_TRACE_ERROR(logger, code)` 只附源码位置并返回原错误，不隐藏 return 或赋值。logger 显式传递，诊断不直接写终端。定义见 [error.h](../src/util/error.h)。

## 8. 日志与可观测性

| 项目 | 当前行为 |
| --- | --- |
| 日志 | debug/info/warn/error，写配置的日志文件；5 MiB 轮转、保留 3 份备份 |
| ReAct | 会话保存执行轨迹，`/trace` 和历史接口读取 |
| 请求与上下文 | 保存模型请求快照、压缩尝试、摘要来源和诊断信息 |
| 用量与计费 | 记录供应商实际提供的 input/output/cached/reasoning 等 usage、phase 及 credit events |
| 工具与进程 | 结构化开始/完成/错误、产物、进程状态和恢复元数据 |
| 子代理与任务 | 保存任务状态、事件、会话关联及通知 |
| HTTP 调试 | `MORPH_DEBUG=1` 启用请求/响应调试记录 |

## 9. 测试与验证

### 9.1 构建与运行

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

- 测试默认启用，GoogleTest 用于 C 库和 Mock LLM 回归。
- Python/PTY 集成覆盖 CLI 呈现、命令、问答、粘贴、权限及 FastCGI API。
- `-DENABLE_ASAN=ON` 同时开启 ASan/UBSan。
- `-DENABLE_READLINE=OFF` 支持独立验证 plain 输入与问答路径。
- `-DENABLE_GLOBAL_STATE_AUDIT=ON` 开启额外全局状态检查。
- 平台沙箱、剪贴板、live MCP 等测试按环境条件执行，跳过不计为通过该能力的现场验收。

### 9.2 本实现基线的验证记录

以下记录对应提交 `1d7d8ac`，不是对所有平台、供应商或任意运行负载的保证。

| 验证 | 结果 |
| --- | --- |
| GoogleTest / CTest 单元用例 | 2263 项：2258 通过，5 项因环境条件跳过 |
| 集成与规范检查 | 54 项通过，含 CLI、FastCGI、JS Host API 和源码约束 |
| ASan/UBSan 核心边界组 | 619 项：618 通过，1 项按条件跳过 |
| ASan/UBSan 终端及边界补充组 | 181 项通过 |
| 普通完整问答 PTY | 通过，含 600 汉字自由输入、1600 汉字自定义答案、尾部完整性和取消 |
| ASan/UBSan 无 Readline 问答 PTY | 通过，覆盖长答案、粘贴及 optional 超时 |

核心回归包括 [模型与流式响应](../tests/test_react.cpp)、[模型所有权](../tests/test_model_ownership.cpp)、[Token 预算](../tests/test_model_tokens.cpp)、[BPE](../tests/test_tokenizer.cpp)、[TUI](../tests/test_tui.cpp)、[问答](../tests/test_ask_user.cpp)、[进程恢复](../tests/test_exec_process.cpp)、[checkpoint](../tests/test_checkpoint_store.cpp)、[记忆分页](../tests/test_runtime_query.cpp) 和 [长输入 PTY](../tests/test_cli_select_pty.py)。

## 10. 资源边界

| 范围 | 默认或固定上限 | 超限行为 |
| --- | --- | --- |
| 项目指令 | `context.project_doc_max_bytes`，默认 32 KiB | 不纳入不完整文件，提示路径和 `file_read` 恢复方式 |
| 提示模板 | 单文件 256 KiB，合并 source / 渲染结果各 1 MiB | 明确错误 |
| 工具历史结果 | `context.tool_result_max_tokens`，默认 8000 | 保持 JSON 有效并标记省略，使用工具恢复入口 |
| 内联进程输出 | `exec.max_inline_output`，默认 32 KiB | 返回游标、spool 路径和范围元数据 |
| 进程内存窗口 | `exec.max_session_output`，默认每会话每输出流 1 MiB | stdout/stderr 分别保留窗口，独立 spool 保存已捕获前缀 |
| 进程 spool | 每个 manager 共 64 MiB 配额 | 明确报告截断、捕获错误和无法恢复的字节范围 |
| MCP stdio 接收帧/缓冲 | 16 MiB | 报错并断连 |
| FastCGI 请求体 | 4 MiB | HTTP 413 |
| Hook 传输 | 输入及各输出流 1 MiB | 明确错误 |
| Hook 内联上下文 | 32 KiB / dispatch，另有每 handler token 策略 | 私有完整文件及可见恢复路径 |
| 用户问答 | 最多 64 选项，答案文本总计 1 MiB | 明确错误，不返回静默缩短答案 |
| 记忆分页 | 最多 4000 Unicode 码点，且满足工具 token 预算 | 完整 UTF-8 页与继续偏移 |
| Skill | 名称 64 / 描述 1024 码点，metadata 16 项，frontmatter 1 MiB | 校验错误 |
| 动态工具源代码 | 配置预算，默认 256 KiB | 明确错误 |
| 动态工具 stdout / Host 捕获 | 配置预算，默认 1 MiB | 超限报错；runner 顶层 stderr 另作有界诊断预览 |
| 子代理 | 同时活跃 8 项，fanout 单批 8 项，嵌套深度 2 | 启动前拒绝超限批次 |
| patch 输入 | 64 KiB / call | 明确错误，按完整 patch 拆分 |
| checkpoint 文件 | 16 MiB / file | 捕获或写前校验失败 |
| checkpoint 数据 | before/after 总计 128 MiB | 写前或 seal 校验失败 |
| checkpoint restore plan | 含当前补偿副本共 256 MiB | 明确错误 |
| 图像解码 | 编码文件 64 MiB，33,554,432 像素，最大边 32768 | 解码前明确拒绝 |
| BPE 词表 token | 最多 65535 字节 | 拒绝加载超出表示范围的词表 |

工具调用身份、动态描述/schema 和用户任务正文不使用旧固定数组前缀替代完整内容。展示省略、上下文预算、传输限制及持久化预算分别执行各自契约。

## 11. 当前实现完成状态

| 已完成范围 | 实现依据 |
| --- | --- |
| Runtime、ReAct 和结构化模型请求 | `src/runtime/`、`src/agent/react*`、`src/models/llm/` |
| 模型预算、流终态、完整调用身份和长凭据 | `src/models/llm.h`、`src/models/llm/`、`src/agent/react_request.c` |
| 模型历史、重复压缩、来源链与请求诊断 | `src/agent/history*`、`src/persistence/history_store.c` |
| 文字、视觉、图片和视频生成 adapter | `src/models/`、`src/agent/tools/` |
| 权限、审批、Hooks、沙箱与进程会话 | `src/permission/`、`src/runtime/approval.c`、`src/runtime/hooks*`、`src/sandbox/`、`src/exec/` |
| 记忆、显式偏好、Plan、子代理及任务 | `src/agent/`、`src/persistence/`、`src/runtime/task*` |
| MCP、Ext、QuickJS、WASM 及 Skill | `src/mcp/`、`src/ext/`、`src/js_runner/`、`src/skill/` |
| patch 跟踪和代码/对话回退 | `src/agent/patch.c`、`src/runtime/checkpoint.c`、`src/persistence/checkpoint_store.c` |
| CLI/TUI、FastCGI 和统一事件 | `src/sapi/`、`src/tui/`、`src/event/` |
| 长输入安全与边界回归 | `src/tui/widgets/editor.c`、`src/util/bpe.c`、`tests/` |

## 12. 已实现行为的边界

- API 失败使用已配置的重试次数和超时；模型可用能力仍由所选 adapter 与供应商决定。
- 本地 token 计数是预算估计，发送前预留输出与协议空间，不将估计当作供应商 usage。
- 超过 spool 配额或捕获失败的内容可能无法恢复，结果元数据明确区分捕获前缀、内存窗口和缺口。
- checkpoint 文件跟踪限于经过其授权和捕获路径的 `apply_patch`；shell、其他工具和子代理不自动纳入文件回退。
- 回退检查外部修改冲突，不根据 Git diff 推断撤销范围。
- 原生共享库在宿主进程执行，其安全边界不同于受沙箱控制的 exec 扩展。
- OS 沙箱、终端协议及系统剪贴板依赖当前运行平台和环境，按实际支持的路径执行。

## 13. 维护依据

本文以实现为依据；接口变更同步修改对应规格、测试和专项文档。详细契约集中在：

- [Runtime 架构](runtime-architecture.md)
- [权限设计](permissions-design.md)与[权限验收](permissions-acceptance.md)
- [模型能力与资源限制](model-capability-limits.md)
- [上下文与进程恢复](context-and-process-recovery.md)
- [checkpoint](checkpointing.md)
- [事件系统](event-system.md)与[Hooks](hooks.md)
- [QuickJS 工具](quickjs-tools.md)与[定时任务](scheduled-tasks.md)
- [TUI 架构](tui-architecture.md)、[应用布局](tui-application-layout.md)和[盒模型](tui-box-model.md)
- [内存与状态所有权](state-ownership.md)
