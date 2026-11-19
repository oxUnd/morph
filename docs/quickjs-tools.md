# 基于 QuickJS 的 Agent 动态工具机制

Morph 通过 `tool_create` 注册 JavaScript 工具，模型可以在同一次任务的后续步骤调用它。
runtime 为每个会话管理工具定义，独立的 `morph-js-runner` 进程用 QuickJS 执行源码。
工具实际能访问的路径、命令和网络目标取决于动态工具 profile 和宿主权限策略。
[注册接口](../src/agent/tools/dynamic_tools.c) · [执行器](../src/js_runner/main.c)

## 已实现能力

### QuickJS 运行时

- 已将 QuickJS 作为内置依赖构建。
- 新增 `morph-js-runner` 子进程执行 JS 工具。
- runner 的图像能力依赖系统 `pkg-config` 包：`vips`、`cairo`、
  `pangocairo`；WASM 能力通过内嵌 Wasm3 静态库提供。
- runner 通过 stdin/stdout 接收和返回 JSON-RPC。
- runner 支持：
  - 普通 `function run(args)`。
  - `async function run(args)`。
  - Promise drain，避免 async 工具返回空对象。

### 动态工具注册

稳定性更新（2026-10-30）：新版本将权威源码 `source` 嵌入 `tool.json`，以该文件
原子替换作为发布提交点；`tool.js` 保留为兼容镜像。旧格式在更新时自动升级，
checkpoint、恢复与提升使用一致快照。直接修改已升级工具的 `tool.js` 不会改变
已提交版本，应通过管理接口发布。每次执行使用独立源码快照；执行中删除或替换
工具不会释放仍在使用的绑定。子进程通信统一处理双流背压、整体超时和取消。

`tool_create` 的必填字段是 `name`、`input_schema`、`output_schema` 和 `source_js`。
`description` 可选。两个 schema 可以传 JSON 对象，也可以传序列化后的 JSON 字符串。
下面的工具接受文本并返回其大写形式。

```json
{
  "name": "upper_text",
  "description": "Convert text to uppercase.",
  "input_schema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]},
  "output_schema": {"type": "object", "properties": {"text": {"type": "string"}}},
  "source_js": "function run(args) { return { text: args.text.toUpperCase() }; }"
}
```

- 创建成功后立即注册到当前 `tool_registry`。
- 保存到会话目录：`~/.morph/runtime/tools/<session_id>/<tool_name>/`。
- 禁止覆盖内置工具、ext 工具和 MCP 工具；同名动态工具会被更新。
- 每次成功创建/更新都会记录 checkpoint，返回 `checkpoint_id`、`old_hash` 和 `new_hash`。
- 会校验：
  - 工具名格式。
  - 输入和输出 schema 的 JSON。
  - JS 源码大小。
  - JS 源码可被 QuickJS 解析。

`tool_history`、`tool_diff` 和 `tool_rollback` 管理动态工具的版本。

- `tool_history` 列出当前 session 动态工具的 checkpoints。
- `tool_diff` 返回某个 checkpoint 与当前工具文件之间的 unified diff。
- `tool_rollback` 回滚到某个 checkpoint。
- 回滚创建操作会删除该 session 动态工具；如果同名 persistent 工具存在，则恢复 persistent 版本。
- 回滚更新操作会恢复旧 `tool.json` / `tool.js`，重新校验并注册工具。

内置工具 `tool_promote`：

- 将当前会话内的动态工具提升为持久工具。
- 保存到：`~/.morph/tools/<tool_name>/`。
- 后续启动 morph 时会自动加载持久动态工具。

内置工具 `tool_delete`：

- 删除当前 registry 中的动态工具。
- session 动态工具会删除会话工具目录。
- persistent 动态工具会删除持久工具目录。
- 不允许删除内置工具、ext 工具和 MCP 工具。

### ReAct 集成

- CLI 和 FastCGI 共用 runtime 的动态工具注册流程。
- FastCGI/server 形态如果没有显式配置 `dynamic_tools.mode`，默认强制使用 `server`。
- ReAct 每轮 LLM 调用前重新收集 active tools。
- 因此 `tool_create` 刚创建的新工具能在同一次任务后续步骤中被模型直接调用。
- 模型收到的工具描述带来源前缀，例如 `[system built-in]`、`[dynamic session]`、`[dynamic persistent]`、`[mcp]`、`[ext]`。

### Host API

JS 工具当前可用 Host API：

```js
morph.fs.readText(path)
morph.fs.writeText(path, text)
morph.fs.readFile(path)
morph.fs.writeFile(path, bytesOrText)
morph.image.open(input)
morph.image.metadata({ input })
morph.image.resize({ input, output, width, height })
morph.image.crop({ input, output, left, top, width, height })
morph.image.extend({ input, output, top, bottom, left, right, background })
morph.image.rotate({ input, output, angle })
morph.image.compose({ input, output, overlays })
morph.image.convert({ input, output })
morph.image.frame({ input, output, style, caption, padding })
morph.canvas.create({ width, height })
morph.canvas.loadImage({ input })
morph.canvas.toFile({ canvas, output })
morph.canvas.toBuffer({ canvas })
morph.env.get(name)
morph.exec(command)
morph.fetch(url, {headers: {Range: "bytes=0-65535"}})
WebAssembly.instantiate(bytes, imports)
```

动态工具中的相对读取路径以 runtime workdir 为基准，相对写入路径以
runtime 已解析的 output_dir 为基准；`morph.exec()` 在 workdir 中执行。
绝对路径保持不变，并在规范化后接受对应 allowlist 检查。

动态工具不是 Node.js。不要使用 `require()`、`Buffer`、`process`、
`node:` 模块、`fs`/`http`/`https`/`child_process`、`__dirname`、
`__filename` 或 npm 包。图片能力统一走 `morph.image`，画布能力统一走
`morph.canvas`。

`morph.fetch(url)` 返回 Response-like 对象：

HTTP 状态码来自实际响应，`ok` 仅在 2xx 时为真。4xx/5xx 仍返回可读取的响应正文；
`morph.fetch(url, options)` 的 `options.headers` 可传递请求头，例如 `Range`，
以分段读取大资源；服务端必须支持相应的 Range 语义。请求头名称和值会校验，
禁止换行、NUL 等控制字符。连接失败、取消、超时或正文超过配置的捕获预算时抛出异常。
捕获预算与 `dynamic_tools.default_max_output_bytes` 一致，默认 1 MiB。
请求复用项目 HTTP 实现，
接受 runner 截止时间约束，并保留 network capability 和目标 allowlist 检查。

`morph.exec(command)` 成功时返回标准输出；非零退出、信号终止、超时或输出超过
同一配置预算时抛出异常，标准输出和标准错误均检查该预算。
子进程使用独立进程组，超时后终止并回收；输出中的 NUL 不会
截断后续内容。

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

### Image And Canvas APIs

```js
async function run(args) {
	await morph.image.resize({
		input: args.input,
		output: args.output,
		width: 512
	});
	return { output: args.output };
}
```

`morph.image` 是 libvips-backed 的安全子集。常用入口：

- `metadata({ input })`
- `resize({ input, output, width, height })`
- `crop()` / `extract()`
- `extend()`
- `rotate()`
- `compose({ input, output, overlays: [{ input, left, top }] })`
- `convert({ input, output })`
- `frame({ input, output, style, caption, padding })`
- `open(input)`：返回图片 handle，可继续调用有限的链式方法。

```js
async function run(args) {
	const canvas = morph.canvas.create({ width: 256, height: 128 });
	const ctx = canvas.getContext("2d");
	ctx.fillStyle = "#ffffff";
	ctx.fillRect(0, 0, 256, 128);
	ctx.fillStyle = "#111111";
	ctx.font = "24px sans-serif";
	ctx.textAlign = "center";
	ctx.textBaseline = "middle";
	ctx.fillText(args.text, 128, 64);
	morph.canvas.toFile({ canvas, output: args.output });
	return { output: args.output };
}
```

`morph.canvas` 是 Cairo-backed 2D 子集。支持 `create()`、`loadImage()`、
`getContext("2d")`、基础路径/矩形/文字绘制、`drawImage()`、`toFile()`
和 `toBuffer()`。文字绘制使用 PangoCairo，`ctx.font` 默认是
`"10px sans-serif"`，支持常见写法如 `"16px sans-serif"`、
`"bold 24px Arial"`、`"italic 18px serif"` 和
`"bold italic 32px sans-serif"`。`ctx.textAlign` 支持 `left`、`right`、
`center`、`start`、`end`，其中 `start` 和 `end` 当前按从左到右的方向
处理。`ctx.textBaseline` 支持 `top`、`hanging`、`middle`、`alphabetic`、
`ideographic` 和 `bottom`；Pango 未直接提供的 hanging/ideographic
基线使用布局度量近似。当前仍未实现完整浏览器 Canvas 文本 API，如
`measureText`。

```js
async function run(args) {
	const bytes = new Uint8Array(args.bytes).buffer;
	const mod = await WebAssembly.instantiate(bytes, {});
	return { value: mod.instance.exports.add(args.a, args.b) };
}
```

`WebAssembly` 由 Wasm3 执行，当前覆盖 `Module`、`Instance`、
`Memory`、`compile()` 和 `instantiate()` 的常用路径；不包含 WASI、
streaming compile、threads、SIMD 或完整浏览器宿主对象。

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
- `image`
- `wasm`

实际能力由当前 profile 的 `default_capabilities` 决定，模型创建工具
时不声明权限。路径、命令、网络访问再继续受对应 allowlist 约束。

当前实现中：

- `fs_read` 受 `allowed_read_paths` 控制。
- `fs_write` 受 `allowed_write_paths` 控制。
- 文件路径 allowlist 按完整目录边界匹配，授权目录不包含同名前缀的相邻目录；符号链接在检查前解析。
- `shell` / `process` 受 `allowed_commands` 控制。
- `network` 受 `allowed_network` 控制。
- `sharp` / `canvas` / `loadImage` 需要 `image`；读写图片路径仍同时受
  `fs_read` / `fs_write` 和对应 allowlist 控制。
- `WebAssembly` 需要 `wasm`。
- `create_requires_approval` 会通过 `tool_context_check_operation()` 审批创建动作。
- `promote_requires_approval` 会通过 `tool_context_check_operation()` 审批持久化动作。

## 配置

在配置文件中设置 `[dynamic_tools]`：

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
default_capabilities = ["fs_read", "fs_write", "network", "process", "env", "mcp", "model", "shell", "image", "wasm"]
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

`tool.json` 保存工具名称、说明、`input_schema`、`output_schema` 和已发布的 `source`。
两个 schema 以 JSON 字符串存储。[存储实现](../src/agent/tools/dynamic_tool_store.c)

## 已有测试覆盖

当前已有单元测试：

- `tool_create` 创建 JS 工具后可立即调用。
- `tool_history` / `tool_diff` / `tool_rollback` 可查看和回滚动态工具创建/更新。
- `tool_delete` 可卸载 session / persistent 动态工具，并拒绝删除非动态工具。
- server profile 默认拒绝文件读取能力。
- `tool_promote` 后新 registry 能自动加载持久工具。
- `morph.canvas` 可创建画布并写出图片。
- `morph.image` 可 resize、compose、extend、frame 图片并读取 metadata。
- `tool_create` 会拒绝 `require()` / Node 风格 source_js。
- `WebAssembly.instantiate()` 可调用导出函数。
- server profile 默认拒绝 `image` 能力。

可从仓库根目录运行 `build/bin/morph-tests --gtest_filter='DynamicToolsTest.*'`。
[动态工具回归](../tests/test_dynamic_tools.cpp)

## 执行边界

- `morph.tool.call()` 是占位接口，调用会抛出异常。动态工具目前不能通过它调用其他 Morph 工具。
- `sharp`、`canvas` 和 `WebAssembly` 提供本文列出的宿主 API 子集。
- runner 通过 `MORPH_RUNNER_MAX_CAPTURE_BYTES` 接收捕获预算，独立启动时默认
  1 MiB，显式值必须在 `1..INT_MAX` 范围内。超限会报错。
