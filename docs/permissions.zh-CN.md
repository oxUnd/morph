# Morph 权限与沙箱配置

本文记录 Morph 当前的权限模型、Local/Server 模式、可搭配项、互斥项、临时审批、
权限 profile、HITL 关系和常见配置。配置文件默认为
`~/.morph/config.toml`，修改后需要重启 Morph。

## 1. 权限模型

Morph 将权限拆成四层：

1. `bash_exec_enabled` 决定 shell 工具是否存在。
2. `bash_exec_mode` 决定使用 Local 交互策略还是 Server 固定策略。
3. 文件系统沙箱决定进程实际能读、写、删除哪些目录。
4. HITL 控制普通工具调用审批；`bash_exec` 和 `request_permissions` 使用自己的
   内部路径审批，不重复经过通用 HITL。

提示词只负责要求模型正确声明权限，真正的边界由沙箱和运行时策略执行。

所有 capability 路径都按目录处理。路径会展开 `~`、规范化并检查目录包含关系；
profile 中的目录必须在 Morph 启动时已经存在。配置相对路径无效。

### 文件操作语义

| 权限 | 允许的典型操作 | 不自动包含 |
|---|---|---|
| read | 读取文件、遍历依赖 | 写入、删除 |
| write | 创建、覆盖、修改 | 删除、重命名 |
| delete | 删除、重命名源文件 | 创建、覆盖 |
| workspace root | Local profile 下的 write + delete | Server 规则、网络 |

同一目录既要生成又要清理文件，应同时授予 `write` 和 `delete`。跨目录移动通常需要
源目录的 `delete` 和目标目录的 `write`。

## 2. 总开关

```toml
[react]
bash_exec_enabled = true
bash_exec_mode = "local" # local | server
```

- `bash_exec_enabled = false`：不注册 `bash_exec`，权限 profile、Server shell 规则和
  `request_permissions` 都不会产生 shell 能力。
- `bash_exec_enabled = true`：按 `bash_exec_mode` 注册并配置 shell。
- 未显式配置时，代码默认 `bash_exec_enabled = false`、
  `bash_exec_mode = "server"`。

## 3. Local 模式

Local 面向有人值守的开发环境：读取和网络默认开放，写入与删除受目录沙箱限制，
边界外可以交互申请。

### 3.1 默认能力

Local 模式无需审批即可：

- 读取任意本地文件；
- 访问网络；
- 在当前 `workdir`、Morph `output` 目录和 `/tmp` 下写入、删除和重命名；
- 执行命令，不使用 `bash_exec_allowed_commands` allowlist。

Local 模式仍会过滤传给子进程的环境变量。代理和 CA 相关变量可以继承，普通密钥
不会因为网络开放而自动传入 shell。

### 3.2 Local permission profile

```toml
[react]
bash_exec_enabled = true
bash_exec_mode = "local"

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
- `request_tool_enabled`：是否注册主动临时授权工具。
- `workspace_roots`：同时允许写入和删除，适合完全信任的项目工作区。
- `write_paths`：只增加写权限。
- `delete_paths`：只增加删除/重命名权限。

profile 是启动时加载的长期静态配置，不会弹审批，也不会出现在
`/permissions list` 中。最多定义 8 个 profile，每次只能激活一个。名称必须唯一，
`active_profile` 必须引用已定义的名称。

### 3.3 多 profile 切换

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

## 4. Local 临时授权

### 4.1 bash_exec 随命令申请

已知外部目录时，模型应随命令声明最小权限：

```json
{
  "command": "rm -rf ~/Library/Developer/Xcode/DerivedData/Morph-*",
  "sandbox_permissions": "with_additional_permissions",
  "additional_permissions": {
    "file_system": {
      "delete": [
        "/Users/me/Library/Developer/Xcode/DerivedData"
      ]
    }
  },
  "justification": "清理该项目的 Xcode 构建缓存"
}
```

兼容旧调用的顶层 `write_paths`、`delete_paths` 仍可使用，但新调用应优先采用
`additional_permissions.file_system`。

Local 路径审批中，`yes once` 只允许当前这次 `bash_exec`，`session` 为同一命令
principal 保留到进程退出，`no` 拒绝。Local shell 路径授权不会提供永久 `always`
选项；需要长期使用时应改为 profile。

`sandbox_permissions` 取值：

| 值 | Local 行为 | Server 行为 |
|---|---|---|
| `use_default` | 默认目录沙箱 | 固定 Server 沙箱 |
| `with_additional_permissions` | 对声明的外部目录弹审批 | 只能使用 Server 已配置目录，不提升 |
| `require_escalated` | 申请文件系统根目录写入和删除能力 | 直接拒绝 |

`require_escalated` 是文件系统全范围兜底，不等于关闭所有进程限制。仅在窄目录授权
无法工作时使用。

如果默认执行触发沙箱拒绝，工具会返回结构化错误：

```json
{
  "error": {
    "code": "sandbox_denied",
    "retryable_with_permissions": true
  }
}
```

即使 shell 中前面的操作失败、最后一个 `echo` 令退出码变成 0，这个错误仍优先
报告。模型应以相同命令和最小附加权限重试。

### 4.2 request_permissions 主动申请

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
- 该工具只支持 Local 文件系统 write/delete，不申请网络权限。
- `request_tool_enabled = false` 时不注册该工具；`bash_exec` 随命令申请仍然可用。

Local shell 路径审批不会写入永久权限数据库；长期权限应使用 profile。

## 5. Server 模式

Server 面向无人值守和固定策略环境。它不弹运行时路径审批，不允许扩大策略，所有
能力必须在启动前写入配置。

```toml
[react]
bash_exec_enabled = true
bash_exec_mode = "server"
bash_exec_allowed_commands = [
  "cmake",
  "ctest",
  "git status",
  "gh pr *",
]

[react.permissions]
active_profile = ""
request_tool_enabled = false

[react.bash_exec_server]
read_paths = ["@workdir", "@output"]
write_paths = ["@output"]
delete_paths = []
network_access = false
allowed_env = []
```

### 5.1 Server 字段

- `bash_exec_allowed_commands`：命令 allowlist；未匹配的命令直接失败。
- `read_paths`：可读目录，同时限制允许使用的 `cwd`。
- `write_paths`：可创建和修改的目录。
- `delete_paths`：可删除或重命名的目录。
- `network_access`：是否允许网络访问。
- `allowed_env`：允许继承的精确环境变量名。

路径特殊值：

- `@workdir`：当前工作目录；
- `@output`：Morph 输出目录；
- `@tmp`：系统临时目录；
- `*`：文件系统根目录。Server 中应谨慎使用。

Server 路径也可以使用绝对路径，但不接受 `~`；应写成展开后的绝对路径。

命令规则支持：

- `"cmake"`：允许任意 `cmake` 调用；
- `"gh pr *"`：允许此前缀下的命令；
- `"git status"`：精确匹配；
- `"*"`：允许所有命令，不建议在生产环境使用。

`bash_exec_allowed_cwds` 已废弃并忽略；使用 Server `read_paths` 控制 cwd。

### 5.2 Server 默认值

未覆盖时，Server 默认策略是：

```toml
[react.bash_exec_server]
read_paths = ["@workdir", "@output"]
write_paths = ["@output"]
delete_paths = []
network_access = false
allowed_env = []
```

默认命令 allowlist 为空，因此仅开启 `bash_exec_enabled` 并不足以执行 Server 命令。

## 6. 配置搭配矩阵

| 配置组合 | 是否可用 | 实际效果 |
|---|---|---|
| Local + `active_profile = ""` | 可以 | 默认根目录 + 交互式临时授权 |
| Local + 已定义 active profile | 可以 | 默认根目录 + profile + 临时授权 |
| Local + `request_tool_enabled = false` | 可以 | 无主动申请工具；bash_exec 仍可随命令审批 |
| Local + `[react.bash_exec_server]` | 可解析但无效 | Server 固定路径/网络规则不参与 Local 沙箱 |
| Local + `bash_exec_allowed_commands` | 可解析但无效 | Local 自动允许执行命令，目录沙箱仍生效 |
| Server + `active_profile = ""` | 可以 | 只使用 Server 固定规则 |
| Server + 非空 active profile | 不可以 | 启动报错：profile 只支持 Local |
| Server + `request_tool_enabled = true` | 可配置但不注册 | Server 永不提供主动提升工具 |
| Server + `with_additional_permissions` | 不提升 | 路径必须已在 Server 固定规则内 |
| Server + `require_escalated` | 不可以 | 工具直接拒绝 |
| `bash_exec_enabled = false` + 任意 shell 权限配置 | 可解析但不生效 | shell 和 request_permissions 都不注册 |
| profile `workspace_roots` + `write_paths/delete_paths` 重叠 | 可以 | 合并生效；重复路径不会扩大到目录外 |
| profile 路径为 `*`、`@workdir` 或相对路径 | 不可以 | profile 只接受绝对路径或 `~` 路径 |

建议不要同时维护“看起来会生效但实际被当前模式忽略”的配置。Local 配置中省略
`[react.bash_exec_server]` 即可；Server 配置中应将 `active_profile` 设为空并关闭
`request_tool_enabled`，让意图更清楚。

## 7. HITL 与内部权限审批

通用 HITL 配置示例：

```toml
[react]
hitl_enabled = true
hitl_tools = ["img_gen", "config_write"]
hitl_auto_approve_readonly = true
```

`bash_exec`、`request_permissions` 和部分具有内部审批的工具带有
`TOOL_FLAG_INTERNAL_APPROVAL`。因此：

- 把 `bash_exec` 放进 `hitl_tools` 不会产生额外的“是否调用工具”审批；
- Local 默认目录内的普通命令可以直接执行；
- 只有命令申请边界外路径时，才出现 write/delete capability 审批；
- Server 模式永不弹路径审批，越界直接拒绝；
- 这样可以避免一次 shell 调用出现 HITL 和路径审批两次弹窗。

若希望“每条 shell 命令都先问一次”，当前不能通过 `hitl_tools = ["bash_exec"]`
实现；需要单独的命令级审批策略，而不是目录 capability 配置。

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

- Local permission profile；
- 当前 turn/session 的临时 bash grant；
- Server 固定 allowlist。

profile 应直接修改 TOML；临时授权会自动过期；Server 规则修改后重启。

## 9. iOS/Xcode 推荐配置

```toml
[react]
bash_exec_enabled = true
bash_exec_mode = "local"

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

上传 App Store 所需的网络在 Local 模式已开放；Server 模式必须显式设置
`network_access = true`，并根据上传工具要求配置 `allowed_env`。

## 10. 选择建议

- 日常项目目录：依赖当前 workdir；确需跨项目编辑时才用 `workspace_roots`。
- 编译缓存：通常同时配置 write/delete。
- 只生成产物、不应清理的目录：只配置 write。
- 偶尔使用的外部目录：不写 profile，走 turn 临时授权。
- 同一会话重复使用的外部目录：使用 session 临时授权。
- 长期可信工具链目录：使用 Local profile。
- 无人值守服务：使用 Server 固定命令、路径、网络和环境变量 allowlist。
- 不确定时优先 Local + 空 profile，让沙箱拒绝后再申请最小权限。

## 11. 常见错误与排查

### active profile 未定义

`active_profile` 必须与某个 `[[react.permission_profiles]].name` 完全一致。

### Server 启动时报 profile 只能用于 Local

将 `[react.permissions].active_profile` 改成 `""`，或切换为 Local。

### 配置了路径但启动失败

确认目录已经存在，且 profile 使用绝对路径或 `~`。Server 路径必须使用绝对路径
或 `@workdir/@output/@tmp/*`。

### 命令 exit_code 为 0 但返回 sandbox_denied

这是预期行为：复合 shell 中前面的操作被拒绝，后面的命令可能令最终退出码变为
0。应按错误中的提示申请路径后重试，不能把退出码 0 当作整体成功。

### 配置没有生效

确认启动的是包含该权限功能的新二进制，并重启 Morph。旧版本会忽略无法识别的
新配置键。
