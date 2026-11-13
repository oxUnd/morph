# TODO

## DeepSeek DSML 原始工具调用标记的处理

- 状态：待办（设计已讨论，未实现）
- 日期：2026-11-13
- 背景：DeepSeek 模型在长 turn 末尾可能把 DSML 工具调用写入 `content`，例如
  `<｜｜DSML｜｜ calls>...<｜｜DSML｜｜ invoke name="exec">...`。正常 ReAct 只解析
  OpenAI 结构化 `tool_calls`，因此该文本被当作最终答案；子代理返回原始标记，
  父代理收到“乱码”，且模型想执行的工具没有运行。
- 现有防护：`src/agent/react_request.c:909` 只在 compaction 路径检测 DSML；
  `tests/test_react.cpp:4535` 也只覆盖 compaction。

### 设计结论

- 不在 ReAct 或通用 codec 中解析 DSML。
- adapter 只声明 provider 的原始工具协议标记：DeepSeek 填 DSML，其他 adapter 为空。
- runtime 注册或注入通用 output guardrail；规则本身与 provider 无关。
- 复用现有 `react_handle_guardrail_retry()` 让模型重试；其他模型下规则不生效，
  或条件注册时不挂载。

### 待办

- [ ] `struct llm_adapter` 增加 `raw_tool_protocols`（open/close 标记）与 count；
      DeepSeek adapter 填 DSML，其他 adapter 保持 NULL/0。
- [ ] `guardrail_eval_ctx` 增加 markers；必要时增加 `tools_available` / adapter 信息。
- [ ] 新增通用 output guardrail `raw_tool_protocol`：只匹配完整包络，避免 fenced code
      误报；命中返回 FAIL，action 要求使用结构化 tool calls 或正常回答。
- [ ] runtime 选择：优先 eval 时按当前 adapter 惰性判定（支持 `/model` 切换）；
      若采用条件注册，必须在模型切换后重新 apply。
- [ ] 重试耗尽后 fail-closed：设置 `REACT_OUTCOME_LLM_ERROR` / 协议错误，不把原始
      标记落库或交给父代理。
- [ ] 考虑独立 protocol guard 或不可禁用；compaction 的 DSML 检测复用同一 marker 列表。
- [ ] 回归测试：DeepSeek DSML -> 重试；非 DeepSeek -> 不触发；畸形/截断 DSML；
      重试耗尽终态；子代理继承规则。
- [ ] 可选：adapter stream 层过滤 DSML，避免 live child feed 短暂显示原始标记。

### 相关文件

- `src/models/llm/adapter.h`
- `src/models/llm/deepseek.c`
- `src/agent/guardrail.h`
- `src/agent/guardrail.c:882`
- `src/agent/react.c:1151`
- `src/runtime/bootstrap.c:196`
- `src/agent/react_request.c:909`
