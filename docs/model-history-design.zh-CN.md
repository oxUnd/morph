# Morph 模型历史与上下文压缩设计

> 实现状态（2026-08-03）：阶段 1、2、3 已全部落地。`messages` 继续作为用户可见
> transcript；`model_history_items` 增量保存模型历史；自动压缩与 `/compress` 使用
> `history_compactions` 原子 checkpoint；工具自定义 history 视图、运行时 secret 清洗、
> provider-neutral 重放、后台收据、导出/诊断/修复和分类成本统计均已实现。

## 1. 背景

Morph 当前将会话历史和 ReAct 轨迹分开保存：

- `messages` 保存用户输入和最终 assistant 回答；
- `react_traces` 保存 Thought、Action、Observation、工具参数和工具结果；
- 下一轮通过 `agent_session_load_history()` 只加载 `messages`；
- `react_traces` 仅供 `/trace` 和调试使用。

这个边界本身是正确的，但当前 `messages` 不足以还原复杂任务。下一轮模型能够看到
“用户要求做什么”和“最终回答是什么”，却看不到产生该结果所必需的工具调用、关键
结果、失败原因和未完成状态。异常轮次还可能保存 `(no response)` 或将工具结果误当成
最终回答，使后续上下文进一步失真。

本设计参考 Codex 的公开实现：平时维护包含工具调用与工具结果的模型可见历史，接近
上下文上限时再生成领域无关的 handoff summary。Morph 不复制 Codex 的存储格式，
只采用其“完整模型历史、工具结果截断、按阈值压缩、当前环境重新注入”的核心原则。

参考：

- [Codex ContextManager](https://github.com/openai/codex/blob/main/codex-rs/core/src/context_manager/history.rs)
- [Codex compaction](https://github.com/openai/codex/blob/main/codex-rs/core/src/compact.rs)
- [Codex 默认 compaction prompt](https://github.com/openai/codex/blob/main/codex-rs/prompts/templates/compact/prompt.md)

## 2. 目标

1. `history` 是下一轮模型理解此前工作的主要依据。
2. `trace` 继续只承担调试、审计和性能排查，不参与上下文恢复。
3. history 保留有语义的工具调用过程，但不保存无界日志和 UI 噪声。
4. 设计与编码、文档、调研、媒体生成、MCP、日程等具体领域无关。
5. 正常情况下不额外调用模型做逐轮总结。
6. 达到 token 阈值时，通过通用 compaction 生成可继续工作的 handoff summary。
7. 中断、超时、空答案和进程异常退出后，history 仍保持可恢复、可发送给模型。
8. 已有 session 可以无损升级；迁移期间保留现有 CLI 和数据库行为。

## 3. 非目标

- 不将完整 ReAct trace 注入模型。
- 不保存或恢复模型内部 Thought/chain-of-thought。
- 不为 Xcode、Git、飞书、旅行等场景建立核心领域 schema。
- 不用 history 代替长期 memory。memory 继续负责跨较长时间的事实、偏好和经验。
- 不保证无限长 thread 永不损失细节。多次摘要必然有信息损耗，系统应可观测并提示。
- 不恢复后台进程本身；阶段 3 保存后台过程的状态收据，让下一轮知道已观察到的进展。

## 4. 三种数据的职责

| 数据 | 面向对象 | 内容 | 是否进入下一轮模型 |
|---|---|---|---|
| Transcript | 用户和 UI | 用户消息、最终回答、可展示附件 | 是 |
| Model history | 模型 | Transcript + 工具调用 + 截断结果 + compaction | 是 |
| Trace | 开发者 | 完整 ReAct 步骤、原始输出、时序和调试数据 | 否 |

长期 memory 是第四条独立链路。它可以从成功会话中抽取事实和经验，但不能替代同一
thread 内的 model history。

关键约束：

- model history 不能从 `react_traces` 反向构建；
- model history 和 trace 可以消费同一批运行时事件，但必须各自持久化；
- transcript 是 model history 的用户可见投影，不要求包含工具消息；
- `/history` 默认显示 transcript，`/history --model` 才显示模型历史收据。

## 5. 总体数据流

```text
用户输入
  ├─> transcript/messages
  └─> model_history_items: user_message
              │
              v
        assistant tool calls
              │  先持久化 call
              v
           工具执行
          /       \
         v         v
完整调试数据     截断、清洗后的模型结果
react_traces     model_history_items: tool_result
                     │
                     v
              assistant final message
                     │
                     v
             下一轮 Context Builder
                     │
          超阈值？否 ─┴─ 是
                         │
                         v
                  LLM compaction
                         │
                         v
              用户消息预算 + handoff summary
```

## 6. Model history item

### 6.1 类型

第一版支持以下稳定类型：

```c
enum history_item_kind {
	HISTORY_ITEM_USER_MESSAGE,
	HISTORY_ITEM_ASSISTANT_MESSAGE,
	HISTORY_ITEM_ASSISTANT_TOOL_CALLS,
	HISTORY_ITEM_TOOL_RESULT,
	HISTORY_ITEM_COMPACTION_SUMMARY,
};
```

不保存 Thought 和 Reflection。assistant 在一次响应中同时发起多个工具调用时，必须
作为一个 `HISTORY_ITEM_ASSISTANT_TOOL_CALLS` 保存，保持模型 API 的原始分组关系。

### 6.2 内存结构

建议新增 `src/agent/history.h` 和 `src/agent/history.c`：

```c
struct history_item {
	int64_t id;
	int64_t session_id;
	int64_t sequence_no;
	char *turn_id;
	enum history_item_kind kind;
	char *role;
	char *content;
	char *payload_json;
	char *tool_call_id;
	char *provider_call_id;
	char *tool_name;
	char *idempotency_key;
	int token_count;
	int truncated;
	int active;
	int64_t created_at;
	struct history_item *next;
};
```

可变字符串从 session arena 分配；数据库查询结果使用现有 session 层的显式释放模式。
JSON 组装使用 `cJSON` 和 `morph_buf_t`。

### 6.3 工具调用 payload

`assistant_tool_calls` 的 `payload_json` 使用 Morph 自有、provider-neutral 格式：

```json
{
  "content": "optional assistant text",
  "calls": [
    {
      "tool_call_id": "call_local_stable_id",
      "provider_call_id": "provider_call_id",
      "name": "bash_exec",
      "arguments": "{\"command\":\"cmake --build build\"}"
    }
  ]
}
```

`tool_call_id` 是 Morph 稳定 ID，用于事件、输出和 trace 关联；`provider_call_id` 用于
重建发送给 OpenAI-compatible API 的 call/output 配对。模型切换时由 adapter 决定是
否沿用 provider ID，不能假设不同 provider 之间可直接重放私有字段。

### 6.4 工具结果 payload

```json
{
  "tool_call_id": "call_local_stable_id",
  "provider_call_id": "provider_call_id",
  "tool_name": "bash_exec",
  "status": "completed",
  "error_code": 0,
  "content": "model-visible truncated result",
  "artifacts": [],
  "meta": {
    "truncated": false,
    "original_bytes": 1200,
    "stored_bytes": 1200
  }
}
```

`status` 取值：

- `completed`
- `failed`
- `cancelled`
- `timed_out`
- `interrupted`

`ui` 不进入 model history。artifact 只保留种类、路径、尺寸等模型继续工作所需的信息。
完整结构化结果仍可由 output 表或 trace 查询。

## 7. 数据库设计

新增表，不直接扩展 `messages`。`messages` 继续作为 transcript，避免 UI、session 命令
和已有客户端被工具消息污染。

```sql
CREATE TABLE model_history_items (
        id               INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id       INTEGER NOT NULL
                         REFERENCES sessions(id) ON DELETE CASCADE,
        sequence_no      INTEGER NOT NULL,
        turn_id          TEXT,
        kind             TEXT NOT NULL,
        role             TEXT,
        content          TEXT,
        payload_json     TEXT,
        tool_call_id     TEXT,
        provider_call_id TEXT,
        tool_name        TEXT,
        idempotency_key  TEXT,
        token_count      INTEGER NOT NULL DEFAULT 0,
        truncated        INTEGER NOT NULL DEFAULT 0,
        active           INTEGER NOT NULL DEFAULT 1,
        created_at       INTEGER NOT NULL,
        UNIQUE(session_id, sequence_no)
);

CREATE INDEX idx_model_history_active
ON model_history_items(session_id, active, sequence_no);

CREATE INDEX idx_model_history_turn
ON model_history_items(session_id, turn_id, sequence_no);

CREATE INDEX idx_model_history_call
ON model_history_items(session_id, tool_call_id);

CREATE UNIQUE INDEX idx_model_history_idempotency
ON model_history_items(session_id, idempotency_key)
WHERE idempotency_key IS NOT NULL;
```

新增 compaction checkpoint：

```sql
CREATE TABLE history_compactions (
        id                 INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id         INTEGER NOT NULL
                           REFERENCES sessions(id) ON DELETE CASCADE,
        turn_id            TEXT,
        cutoff_sequence_no INTEGER NOT NULL,
        summary_item_id    INTEGER NOT NULL
                           REFERENCES model_history_items(id),
        input_tokens       INTEGER NOT NULL,
        output_tokens      INTEGER NOT NULL,
        trigger_kind       TEXT NOT NULL,
        created_at         INTEGER NOT NULL
);

CREATE TABLE history_compaction_attempts (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id    INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
        turn_id       TEXT,
        trigger_kind  TEXT NOT NULL,
        status        TEXT NOT NULL,
        input_tokens  INTEGER NOT NULL DEFAULT 0,
        output_tokens INTEGER NOT NULL DEFAULT 0,
        error_code    INTEGER NOT NULL DEFAULT 0,
        error_text    TEXT,
        created_at    INTEGER NOT NULL
);
```

旧 item 不删除，只标记 `active=0`。这样可以排查摘要丢失，但 Context Builder 只加载
`active=1` 的 item。compaction 必须在一个 SQLite transaction 中完成：

1. 插入 summary item；
2. 标记 cutoff 之前的被替换 item 为 inactive；
3. 保持选中的用户消息 active，必要时复制到新 sequence；
4. 插入 checkpoint；
5. commit。

任一步失败都 rollback，继续使用原 history。

## 8. 持久化时机

### 8.1 用户消息

在 `agent_turn_begin()` 成功校验后、调用模型前持久化。Transcript 和 model history 在同
一 transaction 中写入，避免只有一侧成功。

迁移期应避免 `agent_turn_finish()` 再次保存同一用户消息。通过 `(session_id, turn_id,
kind)` 或显式 `history_item_id` 保证幂等。

### 8.2 工具调用

模型返回 tool calls 后、创建工具线程前持久化。这样即使进程在工具执行期间退出，也
能知道调用已经开始。

### 8.3 工具结果

工具完成、失败、取消或超时时立即持久化，不等待最终 assistant answer。工具结果进入
history 前统一执行：

1. 结果 envelope finalize；
2. 敏感信息清洗；
3. artifact/reference 提取；
4. token/字节截断；
5. 写入 `tool_result` item；
6. 再加入当前轮模型输入。

持久化内容和同一轮后续模型实际看到的内容必须相同，不能磁盘保存一个版本、模型看到
另一个版本。

### 8.4 最终回答

只有真实、非空的 assistant final 才写入 transcript 和 model history。

以下内容不能保存为 assistant final：

- `(no response)`；
- 最后一次工具结果；
- guardrail 占位文本；
- 内部错误 envelope。

没有 final 时保存 turn 状态和已完成的工具 item 即可。下一轮模型可以从工具调用链继续，
UI 则展示该轮 interrupted/failed 状态。

## 9. 工具结果截断

### 9.1 原则

- 截断发生在进入 model history 之前；
- trace 是否截断由 trace 自己的配置决定；
- 保留错误码、状态、artifact 和结构化引用；
- 文本保留头尾，避免只留下构建日志开头而丢失末尾错误；
- 必须使用共享 UTF-8 API，不能切断多字节字符；
- 截断标识必须对模型可见。

### 9.2 默认算法

1. 估算结果 token；
2. 未超过 `tool_result_max_tokens` 时原样保存；
3. 超过后保留约 60% 头部和 40% 尾部；
4. 中间插入明确标记：

```text
…(tool output truncated: original_bytes=123456, stored_bytes=24576)…
```

5. JSON envelope 先保留顶层状态字段，再截断体积最大的字符串字段；
6. image/audio 原始 base64 不进入 history，只保留 artifact path、mime 和尺寸。

工具可以通过 `meta.history_content` 提供更合适的模型视图，但这只是可选增强。没有适配
器的 MCP/Ext 工具仍使用统一截断算法，因此核心机制不依赖领域。

## 10. History normalization

每次构建模型输入前执行通用 normalization：

1. 每个 active `tool_result` 必须有对应的 `assistant_tool_calls`；
2. 每个 tool call 必须有结果；
3. 发现进程异常留下的悬空 call 时，追加合成结果：

```text
tool execution was interrupted before a result was recorded
```

状态为 `interrupted`，不能伪造成功结果。

4. 删除无法配对的孤立 result，并记录 warning；
5. provider/model 不支持的多模态内容降级为文本 artifact reference；
6. 重新计算 token_count；
7. 当前权限、cwd、工具清单、模型设置和系统提示始终从当前 runtime 重新生成，不从旧
   summary 恢复。

最后一条约束很重要：history 可以记录“此前因权限不足失败”，但不能让旧 history 重新
授予权限。

## 11. Context Builder

`agent_session_load_history()` 改为加载 active `model_history_items`，然后通过 adapter 转为
`struct chat_message`：

```text
当前 system prompt
当前 memory context
当前 runtime/environment/permissions context
active model history items
当前用户输入
```

`struct message_list` 目前只能表达 role/content，无法表达 assistant tool calls 和 tool call
ID。建议逐步将 ReAct 的 session history 改为 `morph_array_t<history_item>`，最终由模型
adapter 构造 `chat_message`。迁移期可以并存：

- 无工具的普通消息继续走 `message_list`；
- 结构化历史走新的 History Builder；
- 两条路径通过统一的 `history_build_chat_messages()` 输出。

不能把 tool call 序列格式化成普通 assistant 文本，例如
`bash_exec({"command":"..."})`。必须恢复为模型 API 的结构化 tool call，结果恢复为
匹配 call ID 的 tool message。

## 12. Compaction

### 12.1 触发

沿用现有配置：

- `summarize_threshold_ratio`：达到上下文比例后触发；
- `compress_target_ratio`：压缩后的目标比例；
- `/compress`：手动触发。

自动 compaction 应在一次模型调用前执行。正在运行工具时不启动独立 compaction，避免
与当前 tool call/output 序列竞争。

### 12.2 输入过滤

compaction 模型只接收 model history，不接收 trace。输入包括：

- user messages；
- assistant messages；
- assistant tool calls；
- 已截断 tool results；
- 上一次 compaction summary；
- 当前 plan 的模型可见结果（如果它已经作为 tool result 存在）。

不包括：

- Thought/reasoning；
- UI payload；
- 原始完整日志；
- 当前动态 system prompt 副本；
- API key、token、authorization header。

### 12.3 通用摘要提示

默认 prompt 应保持领域无关：

```text
You are performing a context checkpoint compaction. Create a concise handoff
summary for another model that will continue this thread.

Preserve:
- the user's current goal and explicit constraints;
- progress and outcomes that were actually observed;
- key decisions and user preferences;
- unresolved errors, open questions, and clear next steps;
- critical references, artifacts, identifiers, and examples needed to continue.

Distinguish observed facts from tentative hypotheses. Do not claim that failed
or interrupted work completed. Do not include secrets. Do not include raw logs
when a concise result is sufficient.
```

摘要是自然语言 handoff，不要求领域 schema，也不要求 evidence JSON。事实准确性主要
来自输入已经是经过规范化的 model history，而不是原始 trace。

### 12.4 Replacement history

成功生成 summary 后，active history 替换为：

1. 在 `compaction_user_message_tokens` 预算内，从新到旧选择真实 user messages；
2. 一条 `compaction_summary`，作为内部 system history item 放在最后；
3. compaction 之后产生的新 item。

summary 使用独立 `kind`，不能伪装成真实用户消息；发送模型时渲染为：

```text
[Conversation handoff summary]
...
```

摘要为空、模型失败或 transaction 失败时，原 history 保持不变。之后可以根据错误类型
重试；只有确认 context window 已无法容纳时，才启用保守 fallback：保留最近完整轮次，
并明确插入“早期上下文未能摘要”的系统提示。不能静默删除。

### 12.5 多次 compaction

新 summary 的输入包含上一版 summary，因此支持递归压缩，但每次都会损失细节。

系统记录：

- compaction 次数；
- 输入/输出 token；
- 被替换 item 数；
- summary item ID；
- 触发原因和失败原因。

同一 thread 达到配置的 warning 次数后提示用户开启新 session，而不是假装上下文仍然
完整。

## 13. 配置

在现有 `[context]` 下增加最少必要配置：

```toml
[context]
summarize_threshold_ratio = 0.8
compress_target_ratio = 0.5
keep_recent_rounds = 6 # 仅作为 compaction 失败 fallback

# 单个工具结果进入模型历史的最大 token 数。
tool_result_max_tokens = 8000

# compaction 后额外保留的真实用户消息总预算。
compaction_user_message_tokens = 20000

# handoff summary 的最大输出 token 数。
compaction_summary_max_tokens = 6000

# 可选；为空时使用内置领域无关 prompt。
compaction_prompt_file = ""

# 达到此次数后提示新建 session；0 表示不提示。
compaction_warning_count = 3
```

不增加 coding/travel/document 等 profile。工具需要更好的 history 表达时，通过统一的
`meta.history_content` 提示模型视图，而不是修改核心配置。

## 14. CLI 和可观测性

### `/history`

- 默认行为保持不变，只显示 transcript；
- `/history --model` 显示 active model history；
- `/history --model --all` 包含 inactive/compacted item；
- 工具结果默认只显示工具名、状态、截断标记和摘要，不展开长文本。

### `/context`

增加：

- active history item 数；
- transcript message 数；
- tool result token 占用；
- 当前 compaction window；
- compaction 次数；
- 最近一次 compaction 输入/输出 token 和状态。

### `/compress`

- 使用与自动 compaction 完全相同的事务路径；
- 输出压缩前后 token 和 item 数；
- 失败时明确说明 history 未改变。

### `/trace`

行为不变。`/trace --from-db` 不能影响 active model history。

## 15. 安全和质量约束

1. model history 写入前执行统一 secret redaction；至少覆盖常见 token、Authorization、
   API key 和 runtime 已知 secret env 值。
2. 工具结果中的二进制和 base64 不进入 history。
3. summary 输入和输出都执行 redaction。
4. summary 不具有权限语义；实际权限只取当前 runtime。
5. summary 不能覆盖当前 system/developer instruction。
6. 模型切换时重新运行 history normalization，不重放 provider 私有 reasoning 数据。
7. 所有 truncation 都必须留下可见标记，禁止静默裁剪。
8. compaction 不删除 transcript 和 trace。

## 16. 迁移与兼容性

### 新 session

从第一轮开始双写：

- `messages`：用户可见 transcript；
- `model_history_items`：模型历史。

### 旧 session

首次加载时：

1. 如果存在 `model_history_items`，直接使用；
2. 如果不存在，从 `messages` 按顺序懒迁移 user/assistant item；
3. 不从 `react_traces` 回填工具调用，避免将 debug 数据错误提升为模型上下文；
4. 迁移完成后写入 session 级 migration 标记或依靠是否存在 item 判断；
5. 原 `messages` 不修改。

旧 session 因此不会自动恢复过去丢失的工具过程，但之后的新轮次会使用完整 history。

### API 兼容

- `message_list()`、session history CLI 和现有前端继续读取 transcript；
- 新增 `history_item_list_active()` 等 API；
- ReAct 逐步迁移到新 History Builder；
- migration 完成前保留读取 `messages` 的 fallback。

## 17. 异常与恢复

必须满足以下不变量：

1. history item 的 `sequence_no` 在 session 内严格递增；
2. 工具调用在执行前落库；
3. 工具结果在完成后立即落库；
4. 重启时为悬空 call 合成 `interrupted` result；
5. 负错误码不能保存成成功 result；
6. 空模型响应不能保存成 assistant final；
7. compaction 是原子操作；
8. summary 失败不能改变 active history；
9. trace 保存失败不能阻止 model history 保存；
10. model history 保存失败应使当前 turn 明确失败，不能继续形成不可恢复的状态。

对于最后一项，工具已经真实执行但结果落库失败时，系统应停止继续调用模型并返回持久化
错误。继续执行会让模型状态与磁盘状态分叉。

## 18. 分阶段实施

### 阶段 1：完整模型历史

目标：不依赖 compaction，先解决下一轮看不到工具过程的问题。

1. 新增数据库表和 session CRUD；
2. 新增 `history_item`、History Builder 和 normalization；
3. 用户消息、assistant tool calls、tool results、final answer 增量双写；
4. 工具结果统一截断；
5. 下一轮优先加载 model history；
6. 旧 session fallback；
7. `/history --model` 和 `/context` 统计；
8. trace 路径保持不变。

### 阶段 2：持久化 compaction

1. 通用 compaction prompt；
2. 自动/手动触发；
3. user-message budget + summary replacement；
4. SQLite 原子 checkpoint；
5. 多次 compaction 和 warning；
6. compaction 失败 fallback；
7. 配置 schema 和示例文档。

### 阶段 3：增强项

1. 工具可选 `meta.history_content`；
2. secret redaction 扩展；
3. provider/model 切换兼容策略；
4. background job/process receipt；
5. history 导出、诊断和修复命令；
6. 按 item 类型统计上下文成本。

实现补充：`/history --diagnose` 只读检查，`/history --repair` 在事务中追加悬空调用的
`interrupted` 结果、停用孤立/非法 item 并重算 token；`/history --export <path>` 导出
JSON。回放时使用 Morph 本地 call ID 重新组成 call/result 对，不复用 provider 私有 ID。
后台状态以 `background_receipt` system item 保存，但不会因此恢复或授予后台进程权限。

阶段 1 和阶段 2 都应独立可交付。阶段 1 完成后，即使没有 compaction，复杂任务恢复也
会明显改善；阶段 2 负责长 thread 的上下文预算。

## 19. 测试计划

### 数据库

- item sequence 和索引；
- session 删除级联；
- 旧 session 懒迁移；
- compaction transaction rollback；
- active/inactive 查询顺序；
- 并发写入或重复 turn ID 的幂等性。

### History Builder

- 普通 user/assistant 对话；
- 单工具和并行工具调用；
- call/output 配对；
- orphan result 清理；
- dangling call 合成 interrupted result；
- provider call ID 恢复；
- 多模态降级；
- token 重新计算。

### 截断

- 短结果不变；
- 长结果保留头尾；
- UTF-8/CJK/emoji 不切断；
- JSON 状态字段保留；
- artifact 不丢失；
- base64 被移除；
- secret 被清洗；
- `truncated` 和原始尺寸准确。

### ReAct 集成

- 下一轮能看到上一轮工具调用和结果；
- 工具失败后下一轮能看到失败原因；
- timeout/cancel 后有匹配结果；
- 空答案不写 final；
- trace 关闭或保存失败不影响 history；
- history 保存失败立即终止 turn。

### Compaction

- 阈值以下不调用摘要模型；
- 阈值以上生成 summary；
- 只保留预算内真实用户消息；
- summary 包含上一版 summary；
- summary 空、网络失败、非法输出时 history 不变；
- transaction 失败时 history 不变；
- 多次压缩后 sequence 和 active 集合正确；
- 当前 system prompt 和权限重新注入；
- `/compress` 与自动路径一致。

### 回归

- 现有 session/history 命令输出不变；
- 现有 3 个模型 backend 不受影响；
- MCP、Ext、内置工具使用同一截断路径；
- ASAN/UBSan/Valgrind 无泄漏；
- 全量测试保持零 warning。

## 20. 验收标准

阶段 1：

1. 完成一个包含多次工具调用的 turn，重启 Morph 后继续提问，模型输入仍包含已配对的
   tool calls/results。
2. 构建超时或用户取消后重启，下一轮模型能看到任务在何处中断。
3. history 中不存在 `(no response)` 和伪装成 final 的工具 JSON。
4. `/history` 不显示工具噪声，`/history --model` 可以诊断模型实际看到的 item。
5. trace 完全关闭时，恢复能力仍然成立。

阶段 2：

1. 达到阈值后 active token 降至目标比例附近；
2. compaction 后重启，模型看到真实用户消息和 handoff summary；
3. 原 transcript 和 trace 未被删除；
4. summary 失败不会造成历史丢失；
5. 编码、普通问答、文档处理和 MCP 场景使用同一套核心逻辑。

## 21. 关键设计结论

1. 不做每轮模型总结；平时保存经过清洗和截断的完整 model history。
2. 不做领域化 task-state schema；compaction 使用通用 handoff prompt。
3. `messages` 保持用户 transcript，新表承担模型历史。
4. 工具调用与结果必须结构化恢复，不能转换成普通文本。
5. model history 增量持久化，不能等 turn 结束后一次性保存。
6. compaction 只读取 model history，不读取 trace。
7. trace、transcript、model history 和 long-term memory 各自保持清晰职责。
