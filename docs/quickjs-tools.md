# 基于 QuickJS 的 Agent 动态工具机制

## 当前状态

状态：第一版已实现并提交。

提交：`a540a8c Add QuickJS dynamic tools`

已验证：

- `cmake --build build`
- `cd build && ctest --output-on-failure`
- CLI 场景：agent 使用 `tool_create` 创建 `fetch_baidu`，随后直接调用新工具获取 `www.baidu.com`。
- 验证结果：请求状态 `200 OK`，HTML 长度 `2349`，内容包含 `<title>百度一下，你就知道</title>`。

## 目标

引入一种“agent 运行中自己创建工具”的机制：agent 可以生成 JS 工具代码，morph 使用内置 QuickJS 执行、注册并调用这些工具。

能力不做一刀切限制，而是按运行形态配置：

- local agent 默认可以拥有完整能力。
- server agent 默认收紧能力，需要显式配置放开。

## 已实现能力

### QuickJS 运行时

- 已将 QuickJS 作为内置依赖构建。
- 新增 `morph-js-runner` 子进程执行 JS 工具。
- runner 通过 stdin/stdout 接收和返回 JSON-RPC。
- runner 支持：
  - 普通 `function run(args)`。
  - `async function run(args)`。
  - Promise drain，避免 async 工具返回空对象。

### 动态工具注册

新增内置工具 `tool_create`：

- 参数包括：
  - `name`
  - `description`
  - `args_schema`
  - `source_js`
  - 可选 `capabilities`
- 创建成功后立即注册到当前 `tool_registry`。
- 保存到会话目录：`~/.morph/runtime/tools/<session_id>/<tool_name>/`。
- 禁止覆盖已有工具名，包括内置工具、ext 工具、MCP 工具和已有动态工具。
- 会校验：
  - 工具名格式。
  - `args_schema` JSON。
  - JS 源码大小。
  - JS 源码可被 QuickJS 解析。

新增内置工具 `tool_promote`：

- 将当前会话内的动态工具提升为持久工具。
- 保存到：`~/.morph/tools/<tool_name>/`。
- 后续启动 morph 时会自动加载持久动态工具。

### ReAct 集成

- CLI 初始化时注册动态工具系统。
- FastCGI 初始化时注册动态工具系统。
- FastCGI/server 形态如果没有显式配置 `dynamic_tools.mode`，默认强制使用 `server`。
- ReAct 每轮 LLM 调用前重新收集 active tools。
- 因此 `tool_create` 刚创建的新工具能在同一次任务后续步骤中被模型直接调用。

### Host API

JS 工具当前可用 Host API：

```js
morph.fs.readText(path)
morph.fs.writeText(path, text)
morph.env.get(name)
morph.exec(command)
morph.fetch(url)
```

`morph.fetch(url)` 返回 Response-like 对象：

```js
async function run(args) {
	const response = await morph.fetch("https://www.baidu.com");
	const html = await response.text();
	return {
		ok: response.ok,
		status: response.status,
		length: html.length,
		preview: html.substring(0, 200)
	};
}
```

为兼容早期写法，`morph.fetch(url)` 的返回对象也提供：

- `body`
- `content`
- `length`
- `slice()`
- `substring()`
- `indexOf()`
- `toString()`

### 能力和权限模型

能力项：

- `fs_read`
- `fs_write`
- `network`
- `process`
- `env`
- `mcp`
- `model`
- `shell`

实际能力取交集：

- 动态工具声明的 `capabilities`。
- 当前 profile 允许的 `default_capabilities`。
- 路径、命令、网络 allowlist。

当前实现中：

- `fs_read` 受 `allowed_read_paths` 控制。
- `fs_write` 受 `allowed_write_paths` 控制。
- `shell` / `process` 受 `allowed_commands` 控制。
- `network` 受 `allowed_network` 控制。
- `create_requires_approval` 会通过 `tool_context_check_operation()` 审批创建动作。
- `promote_requires_approval` 会通过 `tool_context_check_operation()` 审批持久化动作。

## 配置

新增 `[dynamic_tools]`：

```toml
[dynamic_tools]
enabled = true
runtime = "quickjs"
mode = "local"              # local | server
session_dir = "~/.morph/runtime/tools"
persistent_dir = "~/.morph/tools"
default_lifetime = "session"
create_requires_approval = false
promote_requires_approval = true
max_source_bytes = 262144
default_timeout_seconds = 30
default_max_output_bytes = 1048576
```

本地模式默认配置：

```toml
[dynamic_tools.local]
default_capabilities = ["fs_read", "fs_write", "network", "process", "env", "mcp", "model", "shell"]
allowed_read_paths = ["*"]
allowed_write_paths = ["*"]
allowed_commands = ["*"]
allowed_network = ["*"]
```

服务端模式默认配置：

```toml
[dynamic_tools.server]
default_capabilities = []
allowed_read_paths = []
allowed_write_paths = []
allowed_commands = []
allowed_network = []
```

## 文件布局

会话工具：

```text
~/.morph/runtime/tools/<session_id>/<tool_name>/
  tool.js
  tool.json
```

持久工具：

```text
~/.morph/tools/<tool_name>/
  tool.js
  tool.json
```

`tool.json` 保存：

- `name`
- `description`
- `args_schema`
- `capabilities`

## 已有测试覆盖

当前已有单元测试：

- `tool_create` 创建 JS 工具后可立即调用。
- server profile 默认拒绝文件读取能力。
- `tool_promote` 后新 registry 能自动加载持久工具。

当前已有人工/CLI 验证：

- 创建 `fetch_baidu` 动态工具。
- 使用 `morph.fetch()` 获取 `https://www.baidu.com`。
- 同一轮 ReAct 中直接调用新创建的动态工具。

## 未完成和后续项

以下内容还没有完整实现，需要后续继续做：

- `morph.tool.call()` 当前只是占位 API，会返回 `morph.tool.call is not available yet`。
- 动态工具调用其他 morph 工具的安全模型尚未完成。
- “动态工具不能绕过 disabled tools”还没有完整链路，因为跨工具调用尚未开放。
- `morph.fetch()` 当前通过 `curl` 子进程实现，状态码目前按成功固定为 `200`，失败由 `curl -f` 转为异常。
- runner 内部 `RUNNER_MAX_CAPTURE` 仍是固定 `1MB`，父进程的 `default_max_output_bytes` 已生效，但 host API 内部抓取上限还未配置化。
- 缺少非法工具名、重复工具名、非法 schema、JS 语法错误、超时、输出大小限制的专项单测。
- 缺少 mock LLM 集成测试覆盖“先 `tool_create`，再调用新工具”的完整 ReAct 流程。
- active tools 目前每轮都会重新收集，后续可以给 `tool_registry` 加 version，只有工具集合变化时才重建。
- 后续需要做工具裁剪，避免动态工具数量过多时增加模型请求 token。

## 默认假设

- 动态工具语言选 QuickJS。
- 本地 agent 可以默认拥有完整能力。
- server agent 默认收紧能力，需要配置显式放开。
- morph 不从自己的服务器下载工具代码。
- 工具代码由本地 agent 生成，保存在用户本机。
