# Tasks and Notifications

## 目标

Tasks 系统用于处理“现在不能立即完成、需要稍后或周期性执行”的用户目标。
它不是低层 cron，也不是固定 tool dispatcher。用户创建的是一个任务，任务到期后
应该重新进入 ReAct loop，由 agent 自己决定是否需要调用工具、调用哪些工具、如何
解释结果，以及如何把最终结果推送给用户。

核心语义：

```text
到时间后，让 agent 执行这个 prompt，并把 final answer 作为通知交付。
```

这意味着即使任务最终只需要调用一个 tool，也应该先经过 agent。这样 agent 可以
处理工具失败、补充推理、结果摘要、格式化通知，以及未来的多步工作流。

## 用户模型

用户看到的是 Tasks：

- 一个 task 有标题、状态、下次运行时间、执行次数和任务内容。
- 一个 task 可以是一次性任务，也可以是周期性任务。
- task 到期后由 agent 执行，不暴露 tool 名称、tool 参数或底层 payload。
- task 的结果进入 inbox；如果 CLI 会话仍然活跃，也可以打印到当前终端。
- task 可以列出、查看详情、更新、取消、手动触发 due processing。

用户不应该需要理解：

- `payload_json`
- `action_type`
- `tool_call`
- scheduler claim 机制
- notification 表结构

## 非目标

当前阶段不做：

- 桌面通知、邮件、IM、webhook 投递。
- 自然语言日期解析器。模型可以把“5 分钟后”转换为 `delay_seconds`，但系统本身
  只接受 Unix 秒或相对秒。
- 独立长期 daemon。CLI 里有轻量 scheduler；未来 daemon 可复用同一套 runner。
- 复杂 watch 条件表达式语言。周期任务每次运行一次 agent，由 agent 判断结果并
  写通知。
- Calendar UI。日历视图可以作为 Tasks 的展示层后续添加。

## 数据模型

现有表结构继续保留，避免过早迁移：

```text
scheduled_tasks
- id
- title
- kind
- status
- trigger_type
- next_run_at
- interval_seconds
- timeout_at
- attempts
- max_attempts
- action_type
- payload_json
- policy_json
- notify_json
- last_error
- created_at
- updated_at

notifications
- id
- task_id
- level
- title
- body
- created_at
- read_at
- delivery_status
```

当前主路径字段约定：

```text
kind = "agent"
action_type = "agent_run"
trigger_type = "once" | "interval"
payload_json = {"prompt":"..."}
```

时间值统一为 UTC Unix seconds。CLI 展示时转换成本地时间。

## 状态机

状态：

- `pending`: 已创建，等待首次运行。
- `running`: scheduler 已 claim，正在执行。
- `waiting`: 已运行过，等待下一次周期运行。
- `completed`: 一次性任务成功完成。
- `failed`: 任务失败且不再重试。
- `cancelled`: 用户取消。
- `timed_out`: 到达 timeout 策略后停止。

一次性任务：

```text
pending
  -> running
  -> completed | failed | timed_out
```

周期任务：

```text
pending
  -> running
  -> waiting
  -> running
  -> waiting
  ...
```

失败时：

```text
running
  -> waiting    如果 interval_seconds > 0 且仍允许重试
  -> failed     如果没有可用重试
  -> timed_out  如果 now >= timeout_at
```

并发防重：

```text
scheduler 查询 due tasks
  -> UPDATE status='running'
     WHERE id=? AND status IN ('pending','waiting')
  -> 只有成功 claim 的 scheduler 可以执行该 task
```

## 执行流程

到期执行：

```text
scheduler finds next_run_at <= now
  -> claim task as running
  -> parse payload_json.prompt
  -> construct background ReAct input
  -> react_run(...)
  -> notification_create(final answer)
  -> once: mark completed
  -> interval: set waiting and next_run_at = now + interval_seconds
```

后台 ReAct 输入会包含任务标题和任务 prompt，并要求 agent：

- 立即执行该 scheduled task。
- 以简洁 notification 的形式返回最终结果。
- 不要追问用户；如果信息不足，说明缺什么以及应如何修改任务。

## CLI Surface

```text
/tasks list [status]
/tasks show <id>
/tasks add <unix_time|+seconds> <prompt>
/tasks every <seconds> <prompt>
/tasks update <id> <unix_time|+seconds> <prompt>
/tasks cancel <id>
/tasks run [limit]
/inbox list [limit]
/inbox read <id>
```

说明：

- `/tasks add +300 搜索今天的 AI 新闻，总结 5 条` 创建 5 分钟后运行的一次性任务。
- `/tasks every 3600 搜索 AI 新闻，总结 5 条重要进展` 创建每小时运行的周期任务。
- `/tasks run` 手动处理当前 due tasks，主要用于调试和非交互场景。
- 交互式 CLI 会启动轻量后台 scheduler，约每秒检查一次 due tasks。

`/tasks list` 默认展示面向人的摘要：

| ID | Status | Schedule | Next run | Attempts | Task |
|---:|---|---|---|---|---|
| 1 | pending | every 3600s | 2026-06-22 15:00:00 | 0/∞ | 搜索 AI 新闻，总结 5 条 |
| 2 | completed | once | - | 1/∞ | 提醒我拿咖啡 |
| 3 | waiting | every 600s | 2026-06-22 15:10:00 | 2/3 | 检查构建是否通过 |
| 4 | failed | every 600s | - | 3/3 | 检查构建是否通过 |

CLI 内部先构造 markdown table，再交给 markdown renderer 渲染，避免固定列宽截断
中文或长 prompt 时破坏 UTF-8。

`/tasks show <id>` 展示 markdown 详情，包括完整 prompt、created/updated 时间、last_error。

## Agent Tool Surface

Agent 可调用 `tasks` tool 管理任务：

```json
{
  "op": "create",
  "title": "Hourly AI news",
  "kind": "agent",
  "trigger_type": "interval",
  "delay_seconds": 3600,
  "interval_seconds": 3600,
  "prompt": "Search AI news and summarize the five most important items.",
  "notify_json": "{\"targets\":[\"inbox\"]}"
}
```

支持 ops：

```text
create
update
list
cancel
inbox
mark_read
```

`run_due` 不暴露给 agent tool。due processing 是 scheduler-only，避免 agent 递归触发
任务执行。

相对时间：

- `next_run_at`: 绝对 Unix seconds。
- `delay_seconds`: 相对当前 user turn start 的秒数。
- 相对时间锚定在用户当前 turn 开始时，而不是模型思考完、调用 tool 的时刻。

## 通知

通知必须持久化，不依赖原 CLI 会话仍然存在。

当前目标：

- `inbox`: 写入 SQLite notifications 表，是唯一可靠交付目标。
- `session`: 如果 CLI 会话还活跃，尽力打印到当前终端；不作为 source of truth。

未来目标：

- `desktop`
- `chat`
- `email`
- `webhook`

notification body 使用 agent final answer。trace、tool observation、内部错误细节不直接
塞进通知正文，避免 inbox 噪音过大。

## 案例

### 1. 一次性提醒

用户：

```text
5 分钟后提醒我拿咖啡
```

agent 创建：

```json
{
  "op": "create",
  "title": "拿咖啡",
  "kind": "agent",
  "trigger_type": "once",
  "delay_seconds": 300,
  "prompt": "提醒用户拿咖啡。"
}
```

到期行为：

```text
agent run -> final: 该拿咖啡了。
notification: info / 拿咖啡 / 该拿咖啡了。
task status: completed
```

### 2. 周期性新闻推送

用户：

```text
每小时搜索一次 AI 新闻，推送 5 条重要进展
```

agent 创建：

```json
{
  "op": "create",
  "title": "Hourly AI news",
  "kind": "agent",
  "trigger_type": "interval",
  "delay_seconds": 3600,
  "interval_seconds": 3600,
  "prompt": "Search current AI news. Summarize the five most important items with links when available."
}
```

每次到期：

```text
agent run
  -> search/news/web tool if available
  -> summarize
  -> notification with final answer
  -> status waiting
  -> next_run_at = now + 3600
```

### 3. 每天背单词

用户：

```text
每天早上 8 点让我背 10 个 GRE 单词
```

当前系统不做自然语言日期解析和 cron。模型需要先换算下一次 8 点的 Unix seconds；
在没有 cron 支持前，可以创建一个 24 小时间隔任务：

```json
{
  "op": "create",
  "title": "GRE vocabulary practice",
  "kind": "agent",
  "trigger_type": "interval",
  "next_run_at": 1782076800,
  "interval_seconds": 86400,
  "prompt": "Prepare a GRE vocabulary practice set with 10 words, definitions, example sentences, and a short quiz."
}
```

到期通知是一次练习内容，不要求用户在线。

### 4. 检查构建直到通过

用户：

```text
每 10 分钟检查这个项目构建是否通过，通过了告诉我
```

创建：

```json
{
  "op": "create",
  "title": "Check build",
  "kind": "agent",
  "trigger_type": "interval",
  "delay_seconds": 600,
  "interval_seconds": 600,
  "max_attempts": 30,
  "prompt": "Run the project build and report whether it passes. If it fails, summarize the most important failure."
}
```

当前语义：

- 每 10 分钟都会运行 agent。
- 如果构建失败，agent 仍然会写一条失败摘要通知，然后任务继续 `waiting`。
- 系统目前没有“agent 判断完成后自动取消周期任务”的结构化 stop contract。

可讨论增强：

```json
{
  "completed": true,
  "message": "Build passed."
}
```

如果后续加入这个结构化 final contract，周期任务可以在 agent 判断完成后自动
`completed`，而不是继续周期执行。

### 5. 检查网页变化

用户：

```text
每 30 分钟看看某个页面有没有发布新版本，有的话通知我
```

创建：

```json
{
  "op": "create",
  "title": "Watch release page",
  "kind": "agent",
  "trigger_type": "interval",
  "delay_seconds": 1800,
  "interval_seconds": 1800,
  "prompt": "Check the release page for new versions. If there is a new version, summarize what changed. If not, say no new release."
}
```

当前限制：

- 没有持久化“上次看到的版本”的专门字段。
- agent 可以在 prompt 中要求对比，但最好后续给 task 增加 `last_result` 或
  `state_json`。

### 6. 任务信息不足

用户：

```text
明天提醒他
```

如果 agent 创建了任务，但 prompt 缺少对象或内容，到期时后台 ReAct 不应该调用
`ask_user` 卡住。它应该输出通知：

```text
这个任务信息不足：没有说明要提醒谁、提醒什么。请更新任务 prompt。
```

状态：

- 一次性任务：当前会按成功 notification 处理并 `completed`。
- 如果希望此类任务进入 `failed`，需要 agent runner 支持结构化失败输出。

### 7. 工具不可用

任务：

```text
每小时搜索新闻
```

如果当前环境没有搜索工具，agent final answer 应说明：

```text
无法完成新闻搜索：当前没有可用的搜索或浏览工具。请启用相关 MCP/tool 后再运行。
```

状态：

- 当前 ReAct 可能仍然返回 final answer，因此任务被视为运行成功。
- 后续可以引入结构化 result，让 agent 明确 `success=false`。

### 8. 取消周期任务

用户：

```text
取消 AI 新闻任务
```

agent 先 list 找到任务，再 cancel：

```json
{"op":"cancel","id":1}
```

状态：

```text
waiting -> cancelled
```

取消后 scheduler 不再处理该任务。

### 9. 查看 inbox

用户：

```text
看看任务通知
```

agent 调用：

```json
{"op":"inbox","limit":20}
```

用户读完后：

```json
{"op":"mark_read","id":42}
```

### 10. 手动触发 due processing

CLI：

```text
/tasks run
```

用途：

- 调试任务执行。
- 非交互命令场景下主动处理 due tasks。
- daemon 未实现前的临时运维入口。

注意：

- 这不是 agent tool 能调用的操作。
- 只处理 `next_run_at <= now` 的任务。

## Task Events

Tasks 已接入统一 event system，事件类型为 `task`。

CRUD 事件：

```text
task.created
task.updated
task.cancelled
```

执行事件：

```text
task.claimed
task.started
task.notification
task.completed
task.rescheduled
task.failed
task.timed_out
task.max_attempts_reached
```

所有 task event 都包含机器可读 payload：

```json
{
  "task_id": 8,
  "title": "Hourly AI news",
  "kind": "agent",
  "trigger_type": "interval",
  "status": "waiting",
  "next_run_at": 1782117600,
  "attempts": 2,
  "max_attempts": 0,
  "reason": "interval",
  "error_code": -5,
  "notification_id": 42
}
```

CLI 的 `/tasks add`、`/tasks every`、`/tasks update`、`/tasks cancel`、
`/tasks run`，以及 agent 调用 `tasks` tool 的 create/update/cancel 都会通过
core event sink 发 task events。Android/iOS 仍需在后续适配中把 task events 接到
现有系统通知/UI 刷新逻辑。

## 当前限制

- 后台 agent runner 复用 CLI 的 `react_context`，通过 `react_lock` 串行化。它能工作，
  但不是最终的多 worker 架构。
- 后台任务当前没有独立 tool registry，因此后台任务仍可能看到交互式工具。prompt 会
  要求不要追问，但后续最好从工具层禁用 `ask_user` 和 `tasks`。
- 没有结构化 agent result contract。现在用 ReAct 返回码判断成功/失败，用 final
  answer 作为 notification body。
- 没有 task-local state，例如 `last_result`、`state_json`、`last_success_at`。
- 没有 cron 表达式；每日/每周任务暂时用 `next_run_at + interval_seconds` 表达。
- 没有 session-scoped/global-scoped 的明确区分；当前任务存储在 Morph SQLite 中。
- Android/iOS 仍然使用 task notification callback；后续应改为消费 `task.notification`
  和 task terminal events。

## 建议的下一步

1. 增加结构化 agent result contract：

```json
{
  "success": true,
  "completed": false,
  "message": "Notification body",
  "next_run_after_seconds": 3600,
  "state": {"last_seen_version":"1.2.3"}
}
```

2. 给 task 增加持久状态字段：

```text
last_result
last_success_at
state_json
```

3. 后台 runner 使用独立 ReAct context 和后台专用 tool registry，默认禁用：

```text
ask_user
tasks
interactive UI tools
```

4. 增加 cron/date parser 层，而不是让 DB 层理解自然语言。

5. 增加 daemon/FastCGI scheduler 入口，复用当前 due runner。
