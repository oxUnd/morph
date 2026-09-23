# Morph 权限与沙箱配置

本文记录 Morph 当前的权限模型、命令审批、权限 profile、HITL 关系和常见配置。
配置文件默认为 `~/.morph/config.toml`，修改后需要重启 Morph。

## 1. 权限模型

Morph 将权限拆成四层：

1. `exec` 工具注册后即可执行命令；它不再是可选工具，也不存在 Shell 模式开关。
2. 每条命令先经过 AST 解析，得到实际要运行的 program 集合。
3. 文件系统沙箱决定进程实际能读、写、删除哪些目录。
4. HITL 控制普通工具调用审批；`exec`、`process` 和 `request_permissions` 使用
   自己的内部命令审批，不重复经过通用 HITL。

提示词只负责要求模型正确声明权限，真正的边界由沙箱和运行时策略执行。

所有 capability 路径都按目录处理。路径会展开 `~`、规范化并检查目录包含关系；
profile 中的目录必须在 Morph 启动时已经存在。配置相对路径无效。

### 文件操作语义

| 权限 | 允许的典型操作 | 不自动包含 |
|---|---|---|
| read | 读取文件、遍历依赖 | 写入、删除 |
| write | 创建、覆盖、修改 | 删除、重命名 |
| delete | 删除、重命名源文件 | 创建、覆盖 |
| workspace root | profile 下的 write + delete | 网络、系统路径 |

同一目录既要生成又要清理文件，应同时授予 `write` 和 `delete`。跨目录移动通常需要
源目录的 `delete` 和目标目录的 `write`。

## 2. Shell 工具与命令审批

Shell 由 `exec` 工具提供，配套 `process` 工具管理长驻会话：

```toml
[exec]
shell = "/bin/bash"
default_timeout_ms = 120000
yield_time_ms = 10000
max_inline_output = 32768
max_session_output = 1048576
kill_grace_ms = 500
network = false
```

字段含义：

- `shell`：求值命令字符串使用的 shell。workdir 在执行前由原生方式应用，不要在
  命令里写 `cd <dir> &&`。
- `default_timeout_ms`：硬超时，`0` 表示禁用。
- `yield_time_ms`：`exec` 在返回运行中的进程会话前等待的时长。
- `max_inline_output` / `max_session_output`：单次响应和单会话的输出保留上限
  （字节）。
- `kill_grace_ms`：SIGTERM 与 SIGKILL 之间的宽限期。
- `network`：默认授予沙箱子进程的网络访问。

不存在 `bash_exec_enabled`、`bash_exec_mode` 之类的开关。`exec` 始终注册；是否
允许某条命令由命令审批和目录沙箱共同决定。如果确实要禁用，用
`[react].disabled_tools` 列出 `exec` 和 `process`。

### 2.1 命令审批

`exec` 收到命令后：

1. 用 AST 解析命令，得到全部 program 名称（`bash -c`、管道、`&&` 等都会被展开）。
2. 若无法安全解析（例如动态生成的命令），直接返回 `approval_required`，不会执行。
3. 以 program 集合作为 principal 计算审批键：单一 program 用其名称，多 program
   用 `"shell"`。
4. 先查本 turn/session 的临时授权，再查持久授权库；命中即放行。
5. 未命中则弹出命令审批，用户可以选择 allow / session / always / deny。

审批裁决为 `allow`、`session`、`always`、`deny` 四种：

- `allow`：仅本次。
- `session`：当前 Morph 进程内对该 principal 与范围有效。
- `always`：写入持久权限库，按项目隔离，可用 `/permissions` 管理。
- `deny`：拒绝，返回 `permission_denied`。

如果运行环境无法交互，审批不可用时返回 `approval_unavailable`。

### 2.2 默认路径

即使命令通过审批，子进程仍受目录沙箱限制。`exec` 会自动授予以下系统路径，以便
常见工具链工作：

```text
只读：/usr  /bin  /sbin  /System  /Library  /opt/homebrew  /private
读+写+删：/tmp
```

`workdir`（未显式给出时取当前工具上下文的工作目录）获得读、写、删除权限。如果
`workdir` 里的 `.git` 是指向别处的文件（git worktree），该 git 目录也会被显式
授予写和删除权。

## 3. Permission profile

```toml
[react.permissions]
active_profile = "developer"
request_tool_enabled = true

[[react.permission_profiles]]
name = "developer"
workspace_roots = [
  "~/Work/AI",
]
write_paths = [
  "~/Library/Caches/my-build-tool",
]
delete_paths = [
  "~/Library/Caches/my-build-tool",
]
```

字段含义：

- `active_profile`：当前启用的 profile 名称；`""` 表示不启用 profile。
- `request_tool_enabled`：是否注册 `request_permissions` 主动申请工具。
- `workspace_roots`：同时允许写入和删除，适合完全信任的项目工作区。
- `write_paths`：只增加写权限。
- `delete_paths`：只增加删除/重命名权限。

profile 是启动时加载的长期静态配置，不会弹审批，也不会出现在
`/permissions list` 中。最多定义 8 个 profile，每次只能激活一个。名称必须唯一，
`active_profile` 必须引用已定义的名称。

profile 只接受绝对路径或 `~` 路径，不接受 `*`、`@workdir` 或相对路径；目录必须在
Morph 启动时已经存在。profile 只增加信任根，永远不会解除操作系统沙箱。

### 3.1 多 profile 切换

```toml
[react.permissions]
active_profile = "strict"
request_tool_enabled = true

[[react.permission_profiles]]
name = "strict"
workspace_roots = []
write_paths = []
delete_paths = []

[[react.permission_profiles]]
name = "ios"
workspace_roots = ["~/Work/iOS"]
write_paths = ["~/Library/Developer/Xcode/DerivedData"]
delete_paths = ["~/Library/Developer/Xcode/DerivedData"]
```

切换只需修改 `active_profile` 并重启。未激活的 profile 不产生权限。

## 4. 临时授权

### 4.1 exec 命令审批

最简单的方式是直接运行命令。如果命令需要沙箱外目录，审批弹窗会显示 principal 和
范围，用户批准后即放行。`exec` 的参数如下：

```json
{
  "command": "cmake --build build",
  "workdir": "/Users/me/Work/AI/project",
  "timeout_ms": 300000,
  "yield_time_ms": 10000,
  "pty": false,
  "background": false
}
```

`workdir` 缺省时回落到当前工具上下文的工作目录；`cwd` 是其兼容别名。

审批失败时返回的 `error.code` 取决于具体原因：

- `permission_denied`：用户拒绝。
- `approval_unavailable`：当前环境没有可用的交互审批通道。
- `approval_required`：命令无法被安全解析，或尚未批准。

另外，`exec` 的工具描述约定：若结果中出现 `error.code=sandbox_denied`，模型应申请
最小的附加能力后重试同一条命令。当前实现里进程启动失败会返回 `spawn_failed`，
workdir 无效会返回 `invalid_workdir`。

### 4.2 长驻进程会话

`exec` 在 `yield_time_ms` 内未结束时返回一个进程会话，附带 `session_id`。随后用
`process` 工具管理：

```json
{ "session_id": "proc_ab12", "action": "poll" }
```

`action` 可取 `poll`、`write`、`interrupt`、`kill`；`write` 通过 `input` 送入
stdin。错误码包括 `process_not_found`、`process_not_running`、`io_error`。

### 4.3 request_permissions 主动申请

模型也可以在执行命令前申请权限：

```json
{
  "command": "xcodebuild archive -scheme App",
  "scope": "turn",
  "permissions": {
    "file_system": {
      "write": ["/Users/me/Library/Developer/Xcode/Archives"],
      "delete": []
    }
  },
  "justification": "生成 Xcode archive"
}
```

- `command` 必须是未来实际运行的完整命令。
- 授权按命令的可执行程序 principal 和目录绑定，不是对所有 shell 命令开放。
- `scope = "turn"` 是默认值，下一轮开始前自动清除。
- `scope = "session"` 保留到当前 Morph 进程退出。
- 该工具只支持文件系统 write/delete，不申请网络权限。
- `request_tool_enabled = false` 时不注册该工具；`exec` 命令审批仍然可用。

该工具的返回值会回显实际生效的 `scope`、`principal` 以及被授予的 `write` /
`delete` 路径。它不会写入持久权限数据库；需要长期授权时应使用 profile，或在
命令审批时选择 `always`。

## 5. 资源上限

沙箱子进程的内存与文件描述符上限由沙箱层控制，不在 `[exec]` 中配置。内存限制
作用于 `RLIMIT_DATA`；对可执行负载会跳过 `RLIMIT_AS`，因为现代运行时（Node.js、
Python、Go）会保留大块虚拟地址空间。core dump 始终禁用，CPU 时间跟随命令超时。
资源上限是尽力而为的系统控制：不支持的项会被记录日志，而文件系统或系统调用
沙箱初始化失败则是致命错误。

底层能力矩阵、Ext manifest 声明和 macOS 嵌套沙箱限制见
[sandbox.md](../sandbox.md)。

## 6. 配置搭配要点

| 配置组合 | 是否可用 | 实际效果 |
|---|---|---|
| `active_profile = ""` | 可以 | 仅 workdir/output/tmp + 交互式审批 |
| 已定义 active profile | 可以 | 额外信任根 + 交互式审批 |
| `request_tool_enabled = false` | 可以 | 无主动申请工具；exec 命令审批仍可用 |
| `workspace_roots` 与 `write_paths`/`delete_paths` 重叠 | 可以 | 合并生效；重复路径不会扩大到目录外 |
| profile 路径为 `*`、`@workdir` 或相对路径 | 不可以 | profile 只接受绝对路径或 `~` 路径 |
| profile 目录在启动时尚不存在 | 不可以 | 启动校验失败 |
| `disabled_tools` 含 `exec` | 可以 | 不注册 exec；`process` 也应一并禁用 |

## 7. HITL 与内部权限审批

通用 HITL 配置示例：

```toml
[react]
hitl_enabled = true
hitl_tools = ["img_gen", "config_edit"]
hitl_auto_approve_readonly = true
```

`exec`、`process`、`request_permissions` 和部分具有内部审批的工具带有
`TOOL_FLAG_INTERNAL_APPROVAL`。因此：

- 把 `exec` 放进 `hitl_tools` 不会产生额外的“是否调用工具”审批；
- 只读工具在 `hitl_auto_approve_readonly = true` 时自动放行；
- 需要沙箱外目录的写/删操作由命令审批处理；
- 无法安全解析的命令直接返回 `approval_required`，不会执行；
- 这样可以避免一次 shell 调用出现 HITL 和命令审批两次弹窗。

若希望“每条 shell 命令都先问一次”，应依赖 `exec` 自身的命令级审批，而不是
`hitl_tools`。

## 8. `/permissions` 命令

```text
/permissions list
/permissions revoke <id|program>
/permissions clear
/permissions clear --yes
/permissions clear --all-projects
/permissions clear --all-projects --yes
```

该命令管理权限数据库中的永久 grant，按项目隔离。它不显示或修改：

- permission profile；
- 当前 turn/session 的临时 grant；
- 沙箱内置的只读系统路径。

profile 应直接修改 TOML；临时授权会自动过期。持久 grant 由命令审批选择
`always` 时写入。

## 9. iOS/Xcode 推荐配置

```toml
[react.permissions]
active_profile = "ios"
request_tool_enabled = true

[[react.permission_profiles]]
name = "ios"
workspace_roots = []
write_paths = [
  "~/Library/Developer/Xcode/DerivedData",
  "~/Library/Developer/Xcode/Archives",
  "~/Library/Developer/Xcode/Products",
  "~/Library/Developer/CoreSimulator",
  "~/Library/Caches/com.apple.dt.Xcode",
  "~/Library/Caches/org.swift.swiftpm",
  "~/Library/org.swift.swiftpm",
  "~/.swiftpm",
]
delete_paths = [
  "~/Library/Developer/Xcode/DerivedData",
  "~/Library/Developer/Xcode/Archives",
  "~/Library/Developer/Xcode/Products",
  "~/Library/Caches/com.apple.dt.Xcode",
  "~/Library/Caches/org.swift.swiftpm",
  "~/Library/org.swift.swiftpm",
  "~/.swiftpm",
]
```

当前项目 workdir 已默认可写，所以通常不必把整个源码父目录加入
`workspace_roots`。CoreSimulator 示例只给 write，不给 delete，可降低误删模拟器的
风险。证书、私钥和 provisioning profile 默认只需读取，不应加入 delete 权限。

上传 App Store 需要网络。默认 `[exec].network = false`，需要时显式开启，并确认
所需环境变量能传给子进程。

## 10. 选择建议

- 日常项目目录：依赖当前 workdir；确需跨项目编辑时才用 `workspace_roots`。
- 编译缓存：通常同时配置 write/delete。
- 只生成产物、不应清理的目录：只配置 write。
- 偶尔使用的外部目录：不写 profile，走 turn 命令审批。
- 同一会话重复使用的外部目录：审批时选 `session`。
- 长期可信工具链目录：使用 profile，或审批时选 `always`。
- 无人值守服务：预先写好 profile，避免运行时弹审批。
- 不确定时优先空 profile，让沙箱拒绝后再申请最小权限。

## 11. 常见错误与排查

### active profile 未定义

`active_profile` 必须与某个 `[[react.permission_profiles]].name` 完全一致。

### 配置了路径但启动失败

确认目录已经存在，且 profile 使用绝对路径或 `~`。profile 不接受 `*`、
`@workdir` 或相对路径。

### 命令返回 approval_required 或 approval_unavailable

`approval_required` 说明命令无法被安全解析（例如经过变量拼接或 `eval`），或用户
尚未批准。`approval_unavailable` 说明当前运行环境没有可用的交互审批通道，例如
无人值守或非交互前端。

### 命令被拒绝但没有明显原因

命令会因为审批未通过而整体失败，而不是只让其中一步失败。别把复合命令里后续
步骤的退出码 0 当作整体成功；先看返回的 `error.code`。

### 配置没有生效

确认启动的是包含该权限功能的新二进制，并重启 Morph。旧版本会忽略无法识别的
新配置键。
