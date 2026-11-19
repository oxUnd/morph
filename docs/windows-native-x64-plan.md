# Morph 原生 Windows x64 移植方案

评审稿 · 2026-11-19 · 源码基线 `83c8ddbc`

## 首发路线建议

如果首发目标是让 Windows x64 用户解压即可使用，建议先交付 **Cygwin 便携包**。现有打包脚本会把 `cygwin1.dll`、Bash、相关 DLL 和运行数据放进发行目录，用户从 Windows Terminal 或 PowerShell 启动 `morph.cmd`，无需另装 Cygwin。这样可以沿用现有 Bash 命令审核、POSIX 进程管理和扩展加载方式。[打包脚本](../scripts/package-cygwin.py)

在当前基线上，按既有构建记录处理 Clang 与 SDK 的头文件兼容问题后，已将 Morph 的进程管理、扩展加载器和 nbase 的 POSIX 进程后端交叉编译为 amd64 COFF 目标文件。当前分支仍需完整链接、打包及 Windows 实机验证。建议把首发工作收敛为一次端到端验收：在干净的 Windows x64 普通用户环境解压，从 `morph.cmd setup` 进入配置，完成一轮对话，并验证 TUI 输入与退出恢复、经授权的 host 命令、后台取消和 runner。[既有构建与 Wine 验证记录](windows-cygwin.md)

决定首发范围的是沙盒。Cygwin 下请求 OS 隔离的命令会失败。显式 host 执行遵循现有授权，并以当前 Windows 用户权限运行。验收需同时证明默认工作区模式拒绝无法隔离的命令，以及单独授权的 host 命令能够执行。若默认 Ask 模式必须能在工作区内隔离运行命令，Windows 沙盒应排入首发。若产品要求去除随包的 Cygwin DLL，再进入下面的完整原生移植路线。

## 完整原生移植的目标

目标是交付可在干净的 64 位 Windows 上解压运行的 `morph.exe` 和 `morph-js-runner.exe`。用户能从 Windows Terminal 启动交互会话，也能从 PowerShell、cmd 或其他程序调用非交互 CLI。发行包独立于 Cygwin、MSYS2 的 POSIX 运行时和开发机的 PATH。现有 Cygwin 构建有自己的产物与验证记录。[Cygwin 构建记录](windows-cygwin.md)

建议首发基线为 **Windows 11 x64、普通用户、本地 NTFS 工作区**，开发与 CI 使用 MSYS2 UCRT64。Windows 10、网络共享工作区和可选的 FastCGI 前端单列兼容性结果。当前已确认的是 nbase 的 Win32 进程源文件可交叉编译为 amd64 COFF，Morph 两个必需模块的原生目标编译分别停在 `sys/wait.h` 和 `dlfcn.h`。整程序链接和原生 Windows 运行仍待实施验证。

## 1. 架构分工

| 所有者 | 负责的工作 | 当前要改的接缝 |
| --- | --- | --- |
| nothing-tui | 唯一的交互终端所有者。管理终端模式、键鼠、粘贴、焦点、尺寸、绘制、事件循环和退出恢复 | 现有聊天页已消费 NT 事件。把仍直接使用 termios、select 和 Readline 回调的交互分支迁入同一套公开 NT 接口 |
| nbase | 跨平台的进程、管道、等待、进程树回收、文件与路径机制。新增的通用机制应有独立的上游用例 | 复用 Win32 的进程和管道实现。补齐实际用到的受控句柄继承、子进程 PTY、动态库加载及安全文件操作，按能力分开提交 |
| Morph CLI 和 runtime | 页面、快捷键的业务含义、会话、后台任务、事件和非交互协议 | 页面接到 Esc 后决定取消输入、打断后台等待或取消当前 turn。runtime 继续拥有任务生命周期和持久化 |
| morph-permission | 解释 access、approval、reviewer、已缓存授权及其有效期，形成有效授权 | 增加 Windows 命令语法与路径身份的适配，同一个授权结果供内置文件工具和子进程消费 |
| morph-sandbox | 将有效授权落实到操作系统，对启动失败、资源生命周期和诊断负责 | 新建 Windows 后端，在父进程准备隔离，以受限身份创建子进程。Windows 后端、Linux Landlock/seccomp 和 macOS Seatbelt 共用上层契约 |

**ConPTY 有两个调用场景。** Morph 的界面运行在 Windows Terminal 提供的控制台中，nothing-tui 已有 Win32 输入、渲染和 ConPTY 测试，Morph 直接消费它的事件。工具调用 `exec(pty=true)` 时，Morph 是另一个程序的宿主，需要为那个子进程分配伪控制台、连接输入输出并回收它。当前固定版 nothing-tui 的 `CreatePseudoConsole` 宿主实现在测试驱动中，nbase v0.1.0 的 Win32 spawn 会拒绝 `pty=true`。建议把这项通用子进程能力放入 nbase，供无界面的执行路径使用，并复用 NT 的测试场景。[nothing-tui 平台说明](https://codeberg.org/oxund/nothing-tui/src/tag/v0.1.0/docs/reference/platforms.md) · [NT 的 ConPTY 测试宿主](https://codeberg.org/oxund/nothing-tui/src/tag/v0.1.0/tests/pty/pty_win.c) · [Morph 工具入口](../src/agent/tools/exec_tool.c)

## 2. 构建、依赖与首个可运行产物

1. 在原生 Windows runner 上使用同一个 UCRT64 前缀的 GCC、CMake、Ninja、pkgconf 和目标库。保留 C11、C++17 与 cleanup 属性检查。固定工具链和包版本，记录下载来源、架构及依赖清单。macOS 上的交叉编译用于尽早发现编译错误。
2. 分目标建立构建矩阵。先编 nbase 和 nothing-tui，再编 QuickJS、wasm3、BLAKE3、tree-sitter、Markdown/MathJax，最后编 `morph-js-runner`、runtime、CLI 和测试。清理 QuickJS 对非 Apple 平台统一链接 `dl` 的假设，验证 Windows 目标的线程依赖、符号导出以及 PE 栈大小。沿用项目固定的源码版本和 companion 源码目录。
   审计 Windows x64 的 LLP64 数据模型。指针、HANDLE、SOCKET、文件偏移与时间戳使用各自的正确宽度，跨 DLL 的回调和格式化参数做编译检查。仓库要求的严格告警继续生效。
3. SQLite、curl、libuv、Readline、libffi、WebP、libxml2、FreeType、HarfBuzz、libvips、Cairo、PangoCairo 按实际 CMake target 探测。已查到 UCRT64 的 Readline、libuv 及图像依赖包，libvips 的包名为 `mingw-w64-ucrt-x86_64-libvips`。CI 以实际配置、链接、运行结果记录可用版本。[现有依赖说明](../README.md#build) · [MSYS2 环境](https://www.msys2.org/docs/environments/) · [UCRT64 libvips](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-libvips)
4. 建立可重复的原生构建、测试、staging 三步。首个产物验收包括 PE32+ AMD64 文件头、两个程序的递归 DLL 导入表、任意工作目录下的 `morph --version` 和 `--help`，以及 runner 的一笔 JSON-RPC 请求。测试环境从 PATH 移除 MSYS2、Git 和开发机 DLL 目录。

## 3. 进程、shell 和工具执行

**进程启动。** 普通模式复用 `nb_process_spawn`、`nb_process_wait`、`nb_process_read` 和限时写入接口。把 Morph 接管 fd 后使用 `fcntl(O_NONBLOCK)` 的逻辑迁回公共接口。保留 stdout/stderr 分流、EOF、输出游标、脱敏、落盘、超时、取消和进程树回收。明确原生中断的语义，分别测试控制台子进程的协作式中断和超时后的强制终止。PTY 模式沿用既有合并输出契约。[Morph 进程管理器](../src/exec/process.c) · [nbase 进程接口](https://codeberg.org/oxund/nothing/src/tag/v0.1.0/include/nb/process.h)

**启动参数。** 当前 Morph 为每次执行传 POSIX 的 `prepare`，非 PTY 执行还传 `new_session`。Windows 使用创建时的环境块、工作目录、句柄白名单和安全属性。沙盒策略由 Morph 持有，nbase 负责创建并返回可等待、可收集、可终止的进程。验证并发启动时每个子进程只收到本次授权的句柄。MCP stdio 改用子进程管道接口，nbase 当前的 Win32 `nb_poll` 只支持 socket。[nbase Win32 启动实现](https://codeberg.org/oxund/nothing/src/tag/v0.1.0/src/platform/win32/process.c) · [MCP stdio](../src/mcp/mcp_stdio.c)

**命令语义。** 建议首发以系统 Windows PowerShell 5.1 为默认候选，P0 先验证它能在拟定沙盒中启动、执行和退出。Windows Terminal、cmd、PowerShell 均可作为启动 Morph 的外壳。Morph 选定的工具 shell、版本、编码和可用工具要进入模型环境描述。历史配置中显式写入的 Bash 路径应保留，缺失时给出可操作的配置错误。

- 已知程序及参数的内部调用直接传 argv。模型的 `exec.command` 保留字符串契约，按选中的 shell 解析。PowerShell 使用与执行引擎匹配的 AST 解析器，可先用固定的只解析辅助程序调用 `Parser.ParseInput`，协议传递原文。权限层接收解析结果、工作目录、网络与宿主请求。解析、审核、执行绑定同一条原文和同一解释器。
- 区分语法错误、可静态识别的程序和运行时才能确定的操作。语法错误明确报出，动态操作按现有共享策略处理。命令名、脚本及缓存授权都要带上方言和解析后的程序身份，避免旧的 Bash 授权隐式扩展到 PowerShell。显式选 Bash 时继续走 tree-sitter-bash。cmd 脚本需要独立验证的解释器规则。
- 一次覆盖 `exec`、hooks、JS runner、扩展及向模型展示的命令示例。验证空参数、引号、尾部反斜杠、中文、空格路径、CRLF、管道和重定向。协议流与二进制内容保持字节透明，控制台展示单独解码。工具 shell 使用无 profile、非交互的启动参数。遇到执行策略限制时返回诊断，并保留系统或企业策略。[当前命令审核](../src/agent/tools/exec_tool.c) · [Hooks](../src/runtime/hooks_executor.c) · [JS 进程调用](../src/js_runner/js_process.c)

## 4. Windows 沙盒

**威胁模型。** 被运行的 shell、脚本、可执行扩展及其后代可能使用调用者未授予的能力。可信边界是 Morph 父进程、操作系统和已经通过 shared permission 生成的授权。沙盒必须在目标程序执行第一条指令前生效。现有 `exec`、可执行扩展、动态工具 runner 各有一个子进程准备回调，Windows 版要从同一受限启动入口进入，hooks 和子代理沿用所属任务的权限上限。[沙盒契约](sandbox.md) · [权限设计](permissions-design.md) · [可执行扩展](../src/ext/ext.c) · [动态 runner](../src/agent/tools/dynamic_tool_runner.c)

**候选实现。** 先在普通用户 Windows VM 上验证 AppContainer，评估 LPAC 对系统默认可读范围的收紧效果。隔离身份限定访问主体，文件 ACL 承载具体路径授权，网络 capability 表达允许的网络范围，Job Object 管理后代、终止和可映射的资源限制。Job Object 单独无法兑现文件或网络隔离。给每个互不信任的并发授权边界独立身份，核算创建、回收和崩溃恢复成本。[AppContainer 隔离](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation) · [Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)

| 契约 | 拟实现的方式 | 原型必须证明 |
| --- | --- | --- |
| 工作区读、写、删除分别授权 | 按有效授权映射 Windows 对象权限，同时考虑目录继承、父目录删除子项的权限、重命名和原子替换 | 在同一棵目录验证只读、可创建/修改、可删除三个边界。受限操作失败后原文件完好，兄弟目录和另一个会话保持各自权限 |
| 临时区、凭据和句柄 | 父进程准备每次任务的私有 scratch、筛选后的 Unicode 环境块、显式继承句柄清单 | TEMP 与 TMP 指向私有目录。并发任务隔离。子进程拿不到模型密钥、父进程日志句柄、SQLite 句柄或其他任务的管道 |
| 网络、IPC、系统运行库 | 默认按业务策略收紧网络和 IPC。分别验证 Internet、局域网、回环、本机服务和命名管道。列出 shell、DLL、字体等执行所需的最小读取范围 | 用真实子进程验证允许和拒绝。宿主仍能正常访问模型。授权网络后可达范围符合该次授权。系统运行库读取不扩展到用户配置 |
| 进程树、资源、退出 | 创建时设置安全属性与句柄继承，采用悬挂创建并完成 Job 绑定后才恢复，或经验证的创建时绑定方式。关闭句柄及错误回滚归同一所有者 | 子孙继承隔离，取消/超时/父进程异常退出均回收进程树。逐项核验内存、CPU、进程数、文件大小和句柄上限，记录操作系统缺少等价机制的项目 |

**原型的决策条件。** 用两份真实工作区测试普通 ACL、自定义/继承 ACL、强制完整性级别和已有文件。先列出每种启动模式需要创建哪些对象、修改哪些安全描述符、何时撤销。临时授权只回收本次添加的项，保留用户并发修改。测试中途失败、应用崩溃、重启恢复以及新文件的继承状态。验证系统默认可读对象与 Morph 明确授权的关系。以扩大用户目录权限或降低原工作区完整性标签为前提的实现，不能作为默认方案。[Windows 文件访问权](https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights)

若原地访问不能同时满足权限精度、普通用户运行和可恢复性，提交第二版设计，比较私有工作区投影与更强的隔离宿主。投影需要评估相对路径、文件监视、并发写、Git 元数据及冲突回写，不能假设与原地构建等价。对于无法落实的有效策略，在启动目标程序前返回明确错误。Full 模式按既有授权直接走 host，隔离初始化失败的请求保留失败结果。[权限验收基线](permissions-acceptance.md)

**启动生命周期。** 父进程先冻结一次权限快照，再构造环境、隔离身份、句柄清单及 Job。操作系统确认隔离成功后，才允许运行用户代码并发布进程已启动事件。每个子进程记录所属 Job、权限范围和对应工具会话。正常退出先收集尾部输出，再按策略收回后代、释放句柄和私有资源。错误路径保留最初的 Windows 诊断，向上返回项目已有错误码。会话活跃时切换到不兼容策略按现有规则返回 busy，向已有会话写入 stdin 仍经过当次权限检查。

## 5. 文件、扩展和交互界面

| 范围 | 移植与验收重点 |
| --- | --- |
| 路径和用户目录 | 内部保留 UTF-8，在 Windows API 边界转换 UTF-16。复用 nbase 的绝对路径、可执行文件路径、临时目录与规范化接口。首发建议将用户目录映射到 `%USERPROFILE%\.morph`，保留 `-c`、`-w`。覆盖盘符、UNC、目录大小写模式、长路径、junction、符号链接、硬链接、设备名及备用数据流。授予范围按实际文件身份和父目录关系判定。明确标注超出初版验证范围的卷类型 |
| Checkpoint、文件工具、SQLite | `apply_patch`、文件授权与 `/rewind` 共用经过验证的路径规则。现有 checkpoint 使用目录 fd、`openat`、`O_NOFOLLOW` 等。Windows 版须用句柄核验父目录、文件身份和 reparse point，让核验和最终文件操作绑定同一目录或文件身份，处理共享锁、并发修改、回滚和部分失败。验证 SQLite WAL 并发读写、会话恢复、备份还原及既有记录读取。[checkpoint 实现](../src/persistence/checkpoint_store.c) |
| DLL 和原生扩展 | nbase 提供可移植加载/查符号/卸载接口，Windows 实现用 `LoadLibraryExW`、`GetProcAddress`、`FreeLibrary`。扩展与 guardrail 共用。用受控绝对路径及 DLL 搜索目录，核验 AMD64、导出名称、调用约定和分配/释放的 ABI 所有权。受限策略对进程内扩展的既有约束继续适用。[扩展加载器](../src/sandbox/loader.c) · [guardrail](../src/agent/guardrail.c) |
| 交互与非交互 | 终端输入、编辑、剪贴板交互、渲染和恢复归 nothing-tui。Morph 保留页面、命令、消息与状态。不再由额外线程与框架争读 stdin。覆盖聊天、setup、审批、提问和长任务取消。重定向 stdin/stdout、`-p` 和 JSON 模式继续使用流式协议，stdout 可供机器逐行解析。[CLI 分层](tui-application-layout.md) · [现有 Esc 监听](../src/sapi/cli/adapters/input/cancel.c) |
| 媒体、MCP 与其他集成 | 覆盖 HTTP/SSE、代理与 CA、MCP stdio 和 Streamable HTTP、JS/Wasm runner、图片编解码、字体与 Markdown。对于 ffmpeg、Git 等外部程序，在实际需要时解析路径并说明缺失能力。每项平台专属集成在启动时按能力发现，分别记录功能验收结果 |

## 6. 实施顺序和里程碑

| 阶段 | 交付内容 | 进入下一阶段的证据 |
| --- | --- | --- |
| P0　验证关键假设 | 建立原生 Windows CI。用小程序分别验证 UCRT64 依赖、nbase 管道与 Job、NT 交互，以及沙盒身份启动真实 PowerShell 并读写受控工作区 | 留存包版本、原生运行日志、权限结果和沙盒原型结论。沙盒方案未定时，构建与界面线可以继续 |
| P1　基础库和链接 | 上游提交 nbase 缺失机制，必要时补 NT 公开接口。发布后固定提交。整理 CMake、平台源文件、路径与 DLL 加载 | 两个 PE 可执行文件在干净环境启动，前述帮助、版本和 runner 协议用例通过。Linux/macOS 同步构建 |
| P2　可用的原生 CLI | 完成 NT 输入迁移、setup、会话、文件工具、checkpoint、MCP、动态 runner、host 进程与后台管理。接入 Windows 命令分析 | 用本地 mock 模型完成完整一轮交互和一轮 JSON 对话，能恢复会话、取消任务、回收后代、重放 checkpoint。记录尚未验收的受限模式 |
| P3　默认权限模式 | 落实选定的 Windows 隔离后端，接通 exec、hooks、可执行扩展、动态工具及后代。P0 之后可与 P1/P2 并行开发 | Ask、Auto、Full、Read Only 和自定义策略均通过。默认 Ask 可实际完成一次工作区任务，隔离失败、无审核回调、撤销和并发边界均有原生证据 |
| P4　发布候选 | 整理 staging、DLL 闭包、运行数据、许可证、安装使用说明和 CI 产物。对发布目录做独立测试 | 在全新 Windows 11 x64 普通用户环境解压即用。发布候选附依赖清单、SHA-256、已验证能力和已知局限 |

**首个合并单元建议是 P0 的 Windows 构建探针与沙盒契约测试。** 先验证 PowerShell 能否在目标身份下可靠启动，再验证真实工作区能否分别授予写权限和删除权限。这两个结果决定 nbase 需要公开哪些启动参数，以及工作区能否原地使用。

## 7. 发布验收

| 用例 | 通过条件 |
| --- | --- |
| 构建与便携包 | 递归检查两个 exe、随包 DLL 和运行时加载模块，全部来自已记录的 x64 来源。检查 `cygwin1.dll`、`msys-2.0.dll` 零依赖。移动安装目录、使用中文用户名、清理开发机 PATH 后仍可运行 |
| 交互 | 原生 ConPTY 自动化覆盖 120/80/60 列、中文输入/粘贴、resize、方向键、Esc、Ctrl+C、审批面板与草稿保留。退出恢复终端。另在真实 Windows Terminal 人工验证输入法、剪贴板及交互观感 |
| 工具与后台 | 模拟模型驱动真实工具。双路大量输出、增量游标、脱敏、stdin、超时、软中断、强制结束、子孙回收、同等策略下 PTY 与管道模式均有断言。stdio MCP 可反复启动/断开，SSE 流支持取消 |
| 权限与隔离 | 正反用例分开记录策略裁决和 OS 结果。Full 无审批适配器可运行。Ask/Auto 缺审批时遵循既有规则。Read Only 在原项目内写入被拒且文件完好。验证独立删除授权、授权过期、网络开关、目录别名、并发任务、父进程崩溃及清理 |
| 数据完整性 | 含中文和空格路径的配置、历史、技能与数据库可读写。分别测试进程中止后的恢复、共享冲突、外部编辑冲突及部分失败后的重试。验证 `apply_patch`、checkpoint 与 `/rewind` 的正常用户入口 |
| 分发 | 随包提供 DLL、`share/morph` 下的 prompts/skills/tiktoken/字体等资产、需要的媒体插件和 CA 配置。实际运行 HTTPS、媒体和 JS/Wasm 用例，覆盖静态导入表看不到的资源。按仓库现有要求核实 nbase 主库再分发授权和各 DLL/字体许可后再公开发包。[依赖分发要求](../AGENTS.md) |

CI 保留三组可区分的结果。每个移植 PR 跑 Linux/macOS 回归和原生 Windows Release 构建、单测、mock 集成与命令行协议测试。能提供桌面控制台的原生 Windows runner 跑 ConPTY 和普通用户沙盒测试，从产物目录启动真实二进制。发布前在全新 VM 再跑一次解压使用，并记录 Windows 版本、终端版本、包清单、跳过用例和原因。已有的 `cli_application_layout_conventions`、`source_control_flow_conventions` 和权限回归继续执行。ConPTY 报告与测试跳过次数分开统计。[CLI 布局与终端回归](../tests/CMakeLists.txt)

评审时最值得先定的是 **首发系统范围、默认 shell、以及沙盒原型对原工作区允许做哪些持久改动**。建议采用上面的 Windows 11 x64 与 PowerShell 候选基线，要求普通用户运行，优先保留原工作区的安全描述符。用 P0 的实测结果决定沙盒实现路线。
