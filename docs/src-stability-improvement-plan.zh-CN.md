# src 稳定性、扩展性与鲁棒性改进设计

- 状态：本次复核发现的实现遗漏已修复，本机可执行门禁通过；完整验证覆盖尚非 100%。
- 文档版本：v4，补充二次复核、错误路径修复及验证边界。
- 日期：2026-10-30。
- 分析基线：提交 `a28338d`；具体行号以该基线为参考，函数名用于后续定位。
- 范围：`src/` 的关键执行路径及相关测试，重点为 runtime、agent、tools、exec、
  ext、MCP、定时任务和 util。本文不代表已对全部源码逐行完成安全审计。
- 授权：用户已明确要求依据本文一次性完成优化并更新状态；本轮覆盖全部 R01–R10。
- 本轮产物：实现、回归测试、项目格式配置及本文完成记录，纳入本次本地 Git 提交。

## 1. 目标与设计约束

目标是提高失败场景下的可预测性：资源有明确所有者，取消和超时能够正确收敛，
工具结果与事件状态一致，部分失败不会破坏已有数据或引起无意的重复执行。

遵循用户提供的《C 语言工程开发规范》、根目录 `AGENTS.md`，并按本节的项目
适配方式纳入 MaJerle/c-code-style。实施约束如下：

1. 使用 C11 和现有静态库边界，保留 runtime facade；前端继续通过
   `struct runtime *` 使用核心能力。
2. 每项实现先确定最小修改范围、API 契约和所有权，再修改行为。
3. 优先修复现有函数。只有重复职责或生命周期边界确实需要时才提取小模块。
4. 不引入可变全局状态、循环依赖、通用任务框架或未经需求验证的扩展机制。
5. 内部辅助函数保持 `static`；跨文件内部接口使用内部头文件。只有调用方确实
   需要时才增加公共 API；生命周期不应暴露的对象使用不透明结构体。
6. 延续负 errno / `MORPH_ERR_*`、显式 `return`、项目容器和 UTF-8 工具。
   错误日志不能替代必要的错误返回；终端输出继续走现有前端协议。
7. 集中清理只用于共享非平凡清理的路径，并在清理前保存原始错误码。
8. 每个变更必须包含针对行为的回归测试，覆盖正常路径和关键错误路径。

适用的 CERT C 检查主题包括分配与释放、错误处理、整数与缓冲区边界、并发数据
竞争和文件资源管理。本文没有进行完整 CERT C 合规认证，也不把源码审阅等同于
动态验证。

### 1.1 外部规范版本与适用范围

本次实际阅读了上游 `CODING_RULES.md`、`.clang-format` 和 `template.h`，参考版本
固定为提交 `9eb29e4aed5b025a42358dcd1b2e38ad2b9cd614`，避免将来 main 分支变化
导致同一设计的判断标准漂移。可定位的原文见第 11 节。

上游首先要求修改现有项目时保持周边代码一致。因此，本版采用“适配到 morph”
的方式，明确哪些规则直接采用、哪些保留项目约定，不宣称逐条原样执行上游规则。
这也延续了用户先前提出的“尊重现有项目架构和编码规范”。

执行顺序为：用户明确指定的项目变更优先，其次为 morph 的现有明确约定，最后
用上游指南补足未约定事项。本文列出的适配表已用于本次修改；将来若明确决定全仓
迁移到上游排版，应另列格式迁移任务并更新项目规则，不能混入稳定性修复。

本轮未导入上游的 AGENTS.md、技能或检查脚本，也未改写 morph 的根目录规则。
新增的 `.clang-format` 使用 morph 自身约定，已用 clang-format 23.1.1 验证。
引用仓库作为规范来源，不意味着采用其自动化环境。

### 1.2 规范冲突与项目适配表

| 主题 | 上游要求或建议 | 本项目执行规则 |
| --- | --- | --- |
| 语言标准 | C11 | 直接采用；保持现有 CMake 设置和 C++17 测试接口 |
| 缩进与行宽 | 4 空格；格式配置为 120 列 | 保留 Tab、宽度 8、软 80/硬 100 列 |
| 函数布局与指针星号 | 返回类型独占行，星号靠类型，函数左括号同行 | 保留 morph 的现有函数定义与 `char *ptr` 风格，不整文件重排 |
| 名称与类型 | 小写下划线，建议私有函数 `prv_` 前缀 | 保留 snake_case、`struct foo` 和现有模块前缀；新增内部函数保持 static，不批量改名 |
| 整数和布尔类型 | 优先定宽整数，以整数表示布尔 | 按语义使用 int/errno、size_t、ssize_t、pid_t、int64_t 等；不为排版替换系统 ABI 类型 |
| 局部变量 | 块首声明，同类型合并并排序 | 新 helper 优先在小作用域块首声明；已有代码不为分组而扩大借用期限或打乱资源获取顺序 |
| 返回路径 | 建议最多两个 return | 保留无清理路径直接返回；有共享非平凡清理时使用项目允许的集中清理，不堆叠 rc 条件链 |
| 条件与空指针 | 指针显式比较 NULL，控制体使用花括号 | 新 helper 采用显式指针判断和花括号；已有局部补丁保持周边风格，不为此扩大修复差异 |
| switch | case 额外缩进，包含 default | 保留项目 case 排版；外部输入有显式非法值处理，内部枚举同时考虑编译器穷尽检查 |
| 状态存储 | 示例允许模块级变量 | morph 的禁止可变静态存储规则更严格，继续执行；不增加 extern 可变全局变量 |
| 释放后置空 | free 后立即置 NULL | 对仍存活的 owner 字段和复用变量置空；局部变量释放后立即退出无需机械赋值；别名仍需独立管理 |
| 函数文档 | 每个函数含 Doxygen，完整文档放定义处 | 新增/实质修改函数补 Doxygen；头文件保留简洁调用契约，定义处说明完整语义，避免重复维护长文 |
| 注释与源字符串 | 英文和块注释 | 与项目一致；本设计文档使用中文，生产 C 源中的注释、错误及日志使用英文 |
| 预处理和头文件 | 条件编译倾向 defined；guard 使用 ifndef | 保留 `MORPH_*_H`、现有条件编译风格及 C++ 兼容，不机械替换 guard |
| 格式工具 | 推荐配套配置及 clang-format 20+ | 固定受支持版本后按 morph 规则适配配置；先只读验证，禁止直接应用上游配置全仓重写 |

表中的差异是明确的项目适配，不是把不同规则混用后宣称全量合规。任何代码风格
调整都不能替代所有权、线程同步、错误处理和业务行为的验证。

### 1.3 纳入每次实现审阅的编码规则

以下规则在本次涉及的新函数和实质修改代码中执行；现有无关代码不做顺手清理。
除上游启发外，安全性约束继续以用户工程规范和 morph 契约为准。

1. **函数与可见性。** 无参数 C 函数明确写 `(void)`；跨模块函数有对应声明，
   源文件优先包含自己的头文件以发现声明不一致；内部实现不进入公共头文件。
   新名称避免保留标识符，不为已有短变量或模块前缀做无关重命名。
2. **类型和边界。** 内存大小、容量和偏移优先使用合适的 size_t/系统类型，
   乘法、加法及有符号转换前检查范围；需要协议固定宽度时使用 stdint 类型。
   使用 `sizeof(var)` 或 `sizeof(*ptr)`，禁止新增 VLA，增长优先用项目容器。
3. **指针和所有权。** 输入允许为 NULL 时先检查；由内部构造保证非空的路径
   写清前提，不要求每层重复检查。只读数据用 const，通用字节载荷按契约采用
   void 指针；C 源中不对 malloc 等 void 指针结果做多余强转。
4. **初始化与控制流。** 结构配置用指定成员初始化，复杂初始化保留尾逗号；
   计数、错误码和布尔状态分别判断，不能把负错误当作普通真值。新增控制体用
   花括号，三目运算不串联副作用；循环形式由退出条件决定，避免为风格改动语义。
5. **常量与宏。** 输出预算、重试次数和宽限期采用有语义的命名常量，不把 0、
   1 等所有字面量机械宏化。优先使用普通函数或 static inline；必需宏保护参数
   和整体表达式，多语句使用 do-while 包装，审查重复求值，不隐藏 return/goto。
6. **头文件与条件编译。** 头文件自包含，标准头用尖括号、项目头用双引号；
   只包含实际需要的类型声明，新增头的依赖位于 extern C 包装之外。保留头保护、
   换行和最小 C/C++ 编译验证，禁止在 C 文件中包含另一个 C 实现文件。
7. **文档与注释。** 注释解释所有权、失败条件和不变量；公共结构成员、回调和
   错误枚举写明语义。新增 Doxygen 使用反斜杠标签；格式化不改变许可证，
   不把上游作者、年份或许可模板当作 morph 的归属信息复制进来。

### 1.4 将编码规则落实到现有问题

| 问题 | 实施时额外检查 | 验证证据 |
| --- | --- | --- |
| R01 工具状态 | rc 的显式比较、错误映射和失败输出契约 | rc/envelope 冲突组合测试 |
| R02/R10 任务状态 | slot 初始化、事务错误保存、条件更新结果 | SQL 故障注入、旧执行身份测试 |
| R03/R04 生命周期 | owned/borrowed 字段注释、引用释放和 const 边界 | 确定性竞争测试、ASan/适用的 TSan |
| R05 进程 I/O | size_t/ssize_t 转换、长度上限和退出清理 | 部分读写、管道背压、取消和超时测试 |
| R06 取消 | 原子访问契约与 token 有效期 | 跨线程及跨回合测试 |
| R07 fanout | pthread 返回码、已启动计数和失败输出 | 创建失败注入与回收计数 |
| R08 guardrail | 明确的检查状态，禁止把错误作为通过 | 各错误策略和非法结果测试 |
| R09 文件 | 写入/关闭返回值、提交点和失败后文件状态 | 部分写入、关闭、rename 和同步失败测试 |

## 2. 证据等级与历史验证基线（实施前）

| 等级 | 含义 | 使用方式 |
| --- | --- | --- |
| 已复现 | 临时程序调用项目实际静态库，观察到具体结果 | 可以据此编写确定性的回归测试 |
| 源码确认 | 已定位实现行为及调用关系 | 不代表已复现崩溃或证明所有运行条件 |
| 待验证风险 | 推导出的竞争时序、故障后果或策略问题 | 先添加复现与故障注入，再确定修复范围 |

前一轮分析已执行 `cmake --build build -j 4`，构建成功；定向执行 45 个测试，全部
通过，涉及 `ScheduledTaskTest`、`RuntimeExecuteTest`、部分 `MockLlmTest` 和
`SubAgentTest`。过滤器中的 `ExtTest.*` 没有匹配实际扩展测试套件，因此这 45 项
不能作为扩展运行路径的验证证据。

两个临时探针分别验证了工具错误结果和任务通知事件失败，结果记录在第 3 节。
临时探针未保留为仓库测试，实施时应转为正式回归用例。

尚未执行：完整 CTest、ASan/UBSan、TSan、Valgrind、跨平台构建，以及本文列出的
故障注入。现有测试通过不等于这些风险已被覆盖或排除。

v2 本轮仅核对外部指南、现有格式约定和本文链接/结构，不重新执行构建或行为
测试；上述 45 项结果仍属于前一轮源码基线，不能记作本轮新增验证。

## 3. 历史问题清单及修复依据（R01–R10 已完成）

P1 表示优先处理的数据一致性、资源生命周期或关键执行错误；P2 表示后续处理的
可靠性和策略完善。优先级不是生产事故已发生的声明。
以下“现状”和“待验证”描述均指实施前基线；完成记录见第 6、10、12 节。

### R01：工具结果的业务状态与返回码不一致（P1，已复现）

位置：`src/agent/tool.c:tool_result_error()`、`tool_exec()`；
`src/agent/react.c:async_tool_exec()`、`react_record_tool_observation()`。

现状：`tool_result_error()` 在成功构造错误 envelope 后返回 0；部分工具直接
返回该值。`tool_exec()` 返回执行函数的 rc，ReAct 又根据 rc 判断成功或失败。

探针行为：注册一个直接返回 `tool_result_error()` 的工具，再通过 `tool_exec()`
执行，观察到 `rc=0`，但结果为 `{"ok":false,"error":...}`。

影响：模型虽然能够看到错误文本，执行事件、失败计数和依赖 rc 的 guardrail
仍可能按成功处理。

最小修改：保留结果构造函数“返回构造状态”的语义，在 `tool_exec()` 边界统一
解释业务失败。优先利用已有 envelope 和 errno，不立即引入一套新的公共结果对象。
对错误 code 的映射集中处理；保留执行函数已经返回的原始负错误码。

必须明确：如何处理缺失/类型错误的 `ok`，以及 `rc<0` 却携带 `ok:true` 的冲突。
建议将冲突视为失败并规范化结果；非法 envelope 返回协议/格式错误，不能默认成功。

### R02：任务状态提交受通知事件回调影响（P1，已复现）

位置：`src/db/scheduled_task.c:run_due_agent()`、`run_due_action()`、
`run_due_reminder()`、`scheduled_task_run_due_collect_with_runner_events()`。

现状：runner 返回后，先创建通知并发送通知事件，再更新任务状态。事件失败会
跳过状态更新。claim/started 事件失败也可能使已经 claim 的任务直接退出处理。

探针行为：创建一次性 agent 任务，让 runner 成功，让 `task.notification` 回调
返回 `-EIO`；观察到 runner 调用 1 次、调度返回 `-EIO`、数据库状态仍为 `running`。

影响：任务的实际执行和数据库状态不一致。过期恢复后可能再次执行已有副作用。

最小修改：runner 在事务外执行；执行后的任务状态和通知记录在同一短事务内提交；
提交后发送通知/完成事件。事件错误必须记录并与执行、持久化错误区分，不能把已
提交的成功任务重新标为待执行。初始 claim 事件失败也需要明确继续/退出语义。

同时检查：通知数组 slot 在 runner 或数据库失败前是否已初始化。当前
`morph_array_push()` 不清零，错误清理却会释放各 slot 的 `body`；正式修改前应
通过数据库失败注入验证，避免失败路径读取或释放未初始化字段。

状态取消与旧执行结果覆盖属于后续条件更新问题，见 R10；本项不直接承诺外部
副作用的 exactly-once，也不立即引入通用消息队列或 outbox 系统。

### R03：工具超时/取消后的借用对象可能提前失效（P1，源码确认，后果待验证）

位置：`src/agent/react.c:struct async_tool_call`、`join_tool_thread()`、
`async_tool_exec()`；`src/runtime/execute.c:runtime_execute()`；
`src/runtime/lifecycle.c:runtime_close()`。

现状：超时和取消路径 detach worker，worker 自行释放 call；但 call 仍借用
react、工具注册表和 output userdata，工具上下文还携带数据库、配置、回调等指针。
部分 worker 回调还会进入栈上构造的 runtime 转发上下文。

风险：call 自身存活不能保证它借用的对象存活；旧任务可能访问已释放对象，或
在下一回合重新绑定上下文后继续产生事件与副作用。现有慢工具取消测试没有覆盖
这些依赖被立即销毁的时序。

最小修改需先在下面两种语义中确定可接受方案：

- 对需要借用回合数据的任务，在返回前完成取消和回收；优点是契约简单，缺点是
  不协作的工具可能延迟回合返回。
- 保留及时返回能力时，由 agent 执行模块跟踪残留任务，独立持有必要数据；回合
  返回前关闭并排空对该请求回调的访问；runtime 最后释放共享依赖。

建议先按第二种方案做小范围设计验证，以保留当前交互体验。不能证明所有借用
对象存活时，不允许仅通过复制外层 struct 或 detach 解决。

约束：C 线程没有安全的通用强制终止方法。禁止用 `pthread_cancel()` 或超时后
直接释放依赖“解决”收敛问题。需要硬超时的非协作执行才考虑子进程隔离，并单独
评估兼容成本。工具超时之后的副作用结果应标为未知，避免误导自动重试。

### R04：工具注册、注销与执行缺少稳定引用边界（P1，源码确认，竞争待复现）

位置：`src/agent/tool.c:tool_lookup()`、`tool_exec()`、`tool_unregister()`；
`src/agent/tools/dynamic_tools.c:tool_delete_exec()`、注册/回滚路径；
`src/agent/react.c:react_start_tool_calls()`。

现状：工具查找返回 entries 中的借用指针；注销立即释放 user_data 和 descriptor，
并移动数组元素；注册可能扩容。ReAct 能并行运行多个工具，其中包含动态修改操作。
现有 tool_search 快照只保护发现过程，不能替代一般执行引用保护。

风险：同批执行、删除、替换工具发生竞争；runtime 的回合锁不能隔离同回合 worker。

最小方向：先验证“串行处理注册表变更”能否在不改变必要行为的前提下解决竞争。
若仍需要执行中注销，则采用不透明稳定引用：查找时 retain，执行结束 release；
注销只移除可发现性，最后一个引用释放后销毁。禁止复制会被独立释放的裸 user_data。

无论采用哪种方案，注册表内部必须统一同步约束；不允许执行端和修改端分别假定
对方持锁。锁内只做短操作，用户回调和析构函数在锁外执行。

### R05：进程 I/O 和超时边界不完整（P1，源码确认，死锁待复现）

位置：`src/ext/ext.c:ext_run()`、`read_fd()`；
`src/mcp/mcp_stdio.c:mcp_stdio_send_bytes()`、`mcp_stdio_exchange()`；
`src/agent/tools/dynamic_tools.c:dynamic_tool_exec()`；
`src/agent/guardrail.c:guardrail_ext_exec_check()`。

现状：extension 先读完 stdout 再读 stderr，子进程大量写 stderr 时可能形成
管道死锁；MCP 阻塞发送完成后才开始接收超时；多处 waitpid/I/O 各自处理错误、
取消和回收。动态工具已有同时读双管道的实现，但发送与最终等待仍需一起检查。

最小方向：先修复实际阻塞路径，再提取已有使用者共同需要的底层 I/O 辅助能力。
提供非阻塞发送、双流读取、统一 deadline、取消检查、输出上限和可靠 waitpid。
JSON-RPC、MCP 协议解析继续归属各自模块。

优先评估 `src/exec/process.c` 的已有机制。它当前面向命令和进程 session，不能
为了复用而把 MCP 请求强行包装成 shell 命令或引入额外权限语义。确需共享时，
提取小型底层 helper；本次最终放在 `morph-util` 的 `child_io.c/h`，避免 ext、
guardrail 和 MCP 引入 exec 的策略依赖，不另建第二套通用进程框架。

### R06：取消状态的线程同步方式不一致（P1，源码确认）

位置：`src/util/cancel.c`、`cancel.h`；`src/util/signals.c`；
`src/agent/react.c:react_cancel()` 及读取路径。

现状：cancel token 使用普通 volatile 读写；部分 signal flag 使用 `__atomic_*`，
react 标记仍有直接读写。`volatile sig_atomic_t` 不能单独提供线程间数据竞争保护。

最小修改：统一项目取消标记的读写实现，审计所有调用点，禁止同一对象混用原子
和非原子访问。先复用已有工具链可用的原子封装，不为此在所有公开结构中暴露
`_Atomic` 并扩大 C/C++ 互操作改动。

取消只表达请求，不负责发布其他数据。纯取消位可以使用 relaxed 原子操作；
任务结果的发布继续通过互斥锁/条件变量完成。重置 token 只允许在旧执行者全部
退出之后进行；每次调用的 token 生命周期必须独立且明确。

deadline 使用单调时钟；线程条件变量的实际时钟支持按平台验证，不假定 macOS
与 Linux 的 pthread 条件变量配置能力完全一致。

### R07：fanout 忽略线程创建和等待失败（P1，源码确认）

位置：`src/agent/sub_agent.c:sub_agent_fanout()`，尤其创建/join 循环。

现状：未检查 pthread_create 返回值，随后无条件 join；未运行的 worker 初始
rc 为 0，可能被统计为成功。pthread 错误应使用其返回值，不能使用 errno 替代。

最小修改：记录实际启动的线程，检查 create/join；部分创建失败时停止继续提交，
等待已启动线程结束，清理结果并返回明确错误。默认不把未提交任务纳入成功结果。
如需部分成功语义，应另行明确结果格式，不在修复中隐式改变。

本项可以用局部变量和清理函数完成，无须先建立线程池或通用 task group。

### R08：guardrail 检查失败被表示为通过（P2，源码确认，策略待确认）

位置：`src/agent/guardrail.c:guardrail_ext_exec_check()`。

现状：扩展空输出、无法启动、协议错误等路径会返回 GUARDRAIL_PASS。
“检查通过”和“没有完成检查”因此无法区分。

最小修改：在内部结果中明确检查错误和原始错误码，再由规则错误策略决定继续、
阻断或交互。保持现有正常 PASS/FAIL 结果和规则行为。

兼容约束：不把现有所有规则自动改为强制阻断。旧配置默认行为、错误可见性、
强制规则如何声明，需在实现前确认；新增配置必须进入现有 schema 验证路径。

### R09：文件关闭错误被忽略，覆盖写缺少一致的提交方式（P2，源码确认）

位置：`src/util/file.c:file_write_all()`；
`src/agent/patch.c:patch_atomic_write()`；
`src/agent/tools/dynamic_tools.c:write_tool_files()`、checkpoint 写入。

现状：file_write_all 直接截断写文件，忽略 fclose 返回值。缓冲写入可能在关闭
时才报错；此时函数仍返回成功。patch 已有临时文件、fsync 和 rename 的局部实现。

最小修改分两步：先检查 close/flush 失败并保留真实错误；再按真实调用需求提取
原子替换 helper，优先迁移动态工具元数据与 checkpoint 等持久数据。

单文件原子替换不等于多文件事务。动态工具源码与元数据的一致发布需要单独设计
版本目录/提交点或可恢复日志，先依据现有恢复需求选择最小方案。

权限模式、符号链接、目标已存在/不存在、临时文件清理和目录 fsync 行为必须
明确；不能在复用 helper 时无意改变现有文件权限或路径审批语义。

### R10：任务完成缺少运行身份和条件更新（P2，源码确认，场景待验证）

位置：`src/db/scheduled_task.c:task_claim_due()`、
`scheduled_task_update_run()`、`scheduled_task_recover_stale_running()`。

现状：claim 依赖状态，最终更新按 task ID 写回；过期恢复按 updated_at 判断，
没有运行批次身份。取消后旧 runner 完成、多个实例共用数据库、长任务被其他实例
恢复时，存在旧结果覆盖新状态或重复运行的风险。

最小方向：先补取消与完成的条件更新测试；确需区分多次运行时增加 run generation，
完成操作必须匹配当前 running 状态及 generation。不匹配返回明确的过期结果。

租约与心跳只在验证多实例/长任务需求后添加，不与 R02 一起扩大成调度系统重写。
外部副作用的去重需要工具或业务提供幂等键，数据库状态更新不能单独实现 exactly-once。

## 4. 模块边界与单向依赖

保持以下职责：

| 模块 | 负责 | 不承担 |
| --- | --- | --- |
| util | 原子取消访问、时间/文件、无策略 fd 通信 | 回合、任务策略、UI 和工具注册 |
| exec / sandbox | 命令进程会话、权限、终止与隔离 | 模型、MCP 协议、工具审批策略 |
| agent | ReAct 状态、工具执行、执行引用及结果解释 | runtime facade 的实现与前端生命周期 |
| tools / ext / MCP | 具体工具及各自协议适配 | 重新实现核心资源生命周期 |
| db / persistence | 数据读写、事务和持久化结构 | 调用模型或运行 agent |
| runtime | 组合上述服务、协调启动/关闭和前端调用 | 前端展示细节 |
| sapi | 输入、展示和前端回调 | 私有数据库/模型/工具对象的独立管理 |

新增依赖保持调用方向从前端到 runtime，再到具体服务及基础模块。agent 内部
执行模块不能反向依赖 runtime；底层进程 helper 不能依赖 tools、agent 或 MCP。

R02 在现有调度函数内修复事务与事件顺序；本次没有迁移整个调度模块。
`child_io` 仅依赖 util 和 POSIX，协议仍由 ext、MCP 和 guardrail 分别处理。

## 5. 已实施的 API 契约

以下按实际实现更新；具体签名与所有权以对应头文件为准。只有实际跨文件调用
所需的接口进入头文件，测试专用线程操作 seam 放在内部头文件中。

### 5.1 所有公共 API 的共同要求

每个新增或改变行为的 API 在头文件中说明：

- 输入是借用还是转移，是否允许 NULL，调用者何时可以释放。
- 返回对象的唯一所有者、对应销毁函数，以及失败时输出参数的状态。
- 能否并发调用、是否需要外部锁、是否允许回调重入、是否可能阻塞。
- 取消和超时表示停止等待还是实际终止，以及部分副作用是否可能已发生。
- 错误码和输出中的业务状态如何配合，失败后哪些资源仍需由调用者释放。

对合法 out 地址，创建操作先置 NULL；失败不交付半初始化对象。带结构体输出的
操作返回可安全 cleanup 的状态。不能对调用者已有资源直接 memset，初始化与
复用契约必须区分。cleanup/destroy(NULL) 在约定支持时为空操作。

文档按第 1.2 节适配：头文件提供可独立理解的简洁调用契约，定义处用英文
Doxygen 说明 `\brief`、`\param[in]` / `\param[out]` / `\param[in,out]`、
非 void 返回值的 `\return`，必要时用 `\note` 说明线程和借用限制。新增 static
函数同样记录关键前提与返回语义；不为未修改的函数批量补注释。

对定义处完整说明和头文件契约的修改必须同批审阅。头文件至少包含所有权、
失败输出、阻塞/并发限制，调用者不应被迫阅读源文件才能安全使用 API。

### 5.2 工具执行边界

继续使用 `tool_exec()` 作为工具执行的统一业务返回边界：

| 项目 | 目标契约 |
| --- | --- |
| args_json | 借用至调用结束；异步提交必须在返回前复制或转移所有权 |
| tool_result | 调用者初始化并最终 cleanup；包括错误情况 |
| 返回值 | 0 表示工具业务成功；负值表示业务或基础设施失败 |
| 错误结果 | 尽可能保留结构化错误；构造失败返回分配错误，结果仍可 cleanup |
| 不存在/禁用 | 明确返回相应负错误，输出不残留上次调用结果 |
| 冲突结果 | 失败优先，统一规范化，避免负 rc 与 ok:true 并存 |
| 构造 helper | 继续返回构造操作状态；调用方不可把它直接等同于业务执行状态 |

禁止仅在 ReAct 某个分支补字符串判断，因为直接调用工具、subagent、MCP 等路径
同样需要一致契约。避免一次性扩展出未使用的复杂错误枚举和重试框架。

### 5.3 工具引用与注册表

注册表内部使用不透明 `struct tool_binding` 引用计数保活 user_data；调用者继续
使用 `tool_exec()`，没有新增公共 acquire/release 类型体系。

| 操作 | 实际契约 |
| --- | --- |
| tool_exec | 短锁内 retain，锁外执行，完成后 release；最后引用销毁 user_data |
| tool_unregister | 移除可发现性和注册引用；已开始执行保持有效；析构在短锁外 |
| tool_register_replace | 预分配描述符后原子发布，失败不接管传入 user_data |
| tool_register_copy / tool_registry_copy | 复制 owned 描述符并共享保活绑定；目标由调用者独占；部分失败仍需 cleanup |
| tool_lookup | 借用视图，需要外部独占或 tool_registry_lock；mutation 后失效 |
| ReAct / discovery 描述符 | 在锁内复制，后续使用 owned snapshot |

动态工具管理操作通过独立 mutation mutex 串行化文件及注册变更，普通工具执行
不持有该锁。引用只保证对象存活，不自动使工具自身 user_data 可重入。注册表
cleanup 要求全部执行者已回收；runtime 关闭顺序已满足这一前提。

### 5.4 异步执行与回调生命周期

执行状态仍由 `react.c` 内部持有，没有为文件大小机械拆模块。取消/超时的 worker
保持 joinable，由 react owner 跟踪。`react_drain_tools()` 是跨文件收敛入口。

资源归属约定：

| 资源 | 所有者和释放时机 |
| --- | --- |
| 调用名、参数、调用 ID | 单次执行对象复制持有至 worker 完成 |
| 回合 ID | 借用 react 上下文；复用或销毁前 drain |
| 取消 token、结果、线程状态 | 单次执行对象持有；旧 worker 退出前不能重置 |
| 工具对象 | 执行引用保活，worker 结束后 release |
| 数据库、模型、配置、logger | 保持现有 owner；销毁前必须确认所有借用执行者已退出 |
| 请求回调和 userdata | 请求调用者持有；execute_turn 返回前禁止新的访问并等待在途回调退出 |
| 已返回回合的残留任务 | agent 执行 owner 持有；runtime 关闭时先取消并收敛 |

对关闭回调通道、等待在途回调、销毁任务三者定义固定顺序；不在持有内部锁时
调用用户回调，不允许回调同步销毁正在调用它的 owner。必要的重入限制要写入
接口注释并由测试验证。

`react_run()` 关闭输出、事件和问答回调，并等待在途回调退出；直接调用者必须
让其余借用依赖存活到 drain/reset/destroy。`runtime_execute_turn()` 在返回和清理
请求绑定前完成 drain，因此非协作进程内工具可能延迟返回。晚到事件和问答返回
`-ECANCELED`；不使用强制线程终止。

### 5.5 底层进程 I/O

`util/child_io.h` 提供 `morph_io_write()` 和 `morph_child_exchange()`，参数只包含
fd、输入、deadline/超时、取消和输出预算，不包含工具、runtime 或 UI 对象。
后者接管 child 和 stdin writer，借用 stdout/stderr 和已初始化缓冲区至返回；
前者借用 fd，恢复原 flags，并在线程内屏蔽和消费自身产生的 SIGPIPE。

- 输入内存借用至通信结束；输出由调用者持有并通过统一 cleanup 释放。
- fd 是借用还是接管必须逐项声明，成功和失败都按相同约定处理，避免重复 close。
- 超时覆盖发送、接收、排空及退出等待；时钟使用单调时钟。
- stdout/stderr 总量受限；截断要有显式标记，并继续按策略排空，不能因不读而死锁。
- EINTR、EAGAIN、部分写入、对端关闭、SIGPIPE、waitpid 错误均有明确处理。
- 停止等待、终止本次子进程、断开长期 MCP 服务是不同操作，不混用语义。
- 原有 process 的 service、notify_only、会话归属和输出保留契约不改变。

### 5.6 任务提交与事件

状态更新和通知记录使用现有数据库连接的短事务；事务内不调用 runner 或前端回调。
首次失败保留原始错误，回滚失败额外记录，禁止返回虚假成功。

事件失败应当可诊断，但与业务执行失败分开。优先使用现有返回/结果机制表达；
确需扩展结果时，以内部结果结构承载 execution、persistence、delivery 状态，
避免同时增加多套公共调度 API。

已增加 `run_token`：claim 成功返回本次身份；finish 必须携带身份并条件更新；
更新任务使旧快照失效；运行中取消也无法被旧 finish 覆写。失效返回 `-ESTALE`。
迁移为兼容旧表的 `TEXT NOT NULL DEFAULT ''` 新列。状态与通知使用同一短
SAVEPOINT 提交；事件回调错误记录日志后继续，不触发已执行任务的重跑。

### 5.7 文件写入

`file_write_all()` 保留普通写入行为并报告 fclose 错误；新增独立
`file_write_atomic()` 供 patch 和动态工具提交使用。现有文件保留权限，新建文件
默认 0600，拒绝符号链接和非普通文件；错误时清理临时文件。

原子替换契约至少包含：同目录临时文件、完整写入、关闭检查、rename 提交点、
权限与符号链接策略、失败清理、提交后同步失败如何报告。rename 成功但目录
同步失败时，新内容可能已可见，API 不能承诺负返回一定表示“未发生修改”。

## 6. 实施完成记录

本次按用户授权完成全部项目，一次性交付；下面是实际修改边界，不是分阶段待办。

| 项目 | 状态 | 实际实现与回归证据 |
| --- | --- | --- |
| R01 | 已完成 | tool_exec 归一业务状态、保留诊断；Stability 与工具/ReAct 既有用例 |
| R02 | 已完成 | 任务状态和通知同事务、事件错误可诊断；StableTask SQL/事件故障用例 |
| R03 | 已完成 | joinable retired workers、回调 gate、runtime 清理前 drain；晚到回调及取消用例 |
| R04 | 已完成 | 绑定保活、短锁、管理操作串行、子注册表持有引用；执行中删除/替换及析构测试 |
| R05 | 已完成 | 共享 child_io、双流排空、整体超时/取消；扩展、动态工具、guardrail、MCP 用例 |
| R06 | 已完成 | 项目原子封装统一取消访问，worker token 独立；并发及回合隔离用例 |
| R07 | 已完成 | create/join 返回值、实际启动计数、失败回收；首个/中途创建失败及 join 注入 |
| R08 | 已完成 | 显式检查错误、on_error 策略及 schema；旧配置继续、block 阻断用例 |
| R09 | 已完成 | fclose 错误、原子替换、动态工具单一元数据提交点；关闭失败及中断恢复用例 |
| R10 | 已完成 | run_token 迁移与 finish 条件更新；取消和旧代迟到无法覆盖新状态 |
| 格式与接口 | 已完成 | morph .clang-format、修改片段格式化、新头文件自包含检查 |

没有引入通用 operation context、线程池、通用 outbox、完整调度租约框架或机械
文件拆分。`child_io` 和 binding 仅抽取已有重复职责与实际生命周期需要。

额外修复：全量 UBSan 发现固定版本 cmark-gfm 的 unsigned 负下标指针运算 UB；
`cmake/FixCmarkGfm.cmake` 在配置后、编译前对匹配语句作一行修正，重复执行无副作用，
源码不匹配时明确失败，避免仅修改构建缓存而无法重现。原上游扩展规范测试用于回归。

## 7. 回归测试矩阵

并发测试优先使用 barrier/条件变量或测试内部注入点控制时序；避免靠固定 sleep
猜测竞争。注入点保持测试内部，不增加生产公共 API 或可变全局状态。

| 对象 | 正常路径 | 必测错误与竞争路径 |
| --- | --- | --- |
| 工具结果 | rc=0 且 ok:true | rc=0/ok:false；负 rc/ok:true；缺字段、非法 envelope；构造 OOM |
| fanout | 全部启动并合并结果 | 首个/中途 create 失败；join 失败策略；已启动任务全部回收 |
| 取消 | 单次取消及时可见 | 并发读写；两个 runtime 隔离；旧 token 不影响下一回合 |
| 异步工具 | 正常完成和超时 | 取消后立即返回并释放请求；立刻关闭 runtime；晚到 stream/问答回调 |
| 注册表 | 注册、查找、执行、注销 | 执行中删除/替换；扩容；同名重新注册；析构恰好一次 |
| extension I/O | 正常 JSON-RPC 和退出 | stderr 超管道容量；边写边读；不读 stdin；不退出；输出超限；对端提前关闭 |
| MCP stdio | 正常请求、响应、通知 | 发送阻塞期间取消/超时；残留响应与下次请求；长期连接断开策略 |
| 任务提交 | runner 成功、状态与通知一致 | claim/notification/completed 回调失败；SQL 写入/提交失败；通知 slot 清理 |
| 任务运行身份 | 一次执行正常完成 | 运行中取消；旧执行者迟到；两个连接竞争；过期恢复后旧结果提交 |
| guardrail | 正常 PASS/FAIL | 启动失败；空/非法响应；超时；每种错误策略及旧配置兼容 |
| 文件 | 空/非空内容及正常替换 | 部分写入、EINTR、关闭失败、空间不足、rename 失败、提交后同步失败 |

涉及源码/元数据的多文件发布时，增加每个提交阶段的中断恢复测试。涉及前端
取消或事件路由时，增加 CLI PTY 和 FastCGI 测试；不因核心单元测试通过而跳过。

## 8. 构建与质量门禁

### 8.1 编译与现有契约

项目已经设置 C11，以及 C 源码的 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`。
本次运行相关测试、完整构建与 CTest，具体结果见第 12 节。

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

构建日志不得新增警告。增量构建没有重新编译的文件不作为零警告审计证据；最终
使用独立 ASan/TSan 构建，并对全部 src C 源执行严格 -Werror 编译前端检查。
保留 JSON 事件、CLI 非交互输出、审批和沙箱语义，明确记录工具错误返回等兼容影响。

### 8.2 Sanitizer 和平台验证

现有 `ENABLE_ASAN` 同时启用 address 和 undefined sanitizer：

```sh
cmake -S . -B build-review-asan -DBUILD_TESTS=ON -DENABLE_ASAN=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-review-asan -j 4
ctest --test-dir build-review-asan --output-on-failure
```

TSan 必须使用独立构建，不能与 ASan 混用。先验证工具链和测试依赖是否支持；
不在没有运行的情况下宣称“无数据竞争”。Valgrind 在支持的平台运行。

平台验证目标为 macOS/Linux；本次实际仅在 macOS arm64 运行。增加源文件时同步维护
`sources.list` 和实际 CMake 来源；若影响 Android/iOS 共用核心清单，验证对应
编译入口。移动端 vendor 快照不在本轮默认修改范围。

### 8.3 交付检查表

- [x] 最小修改范围、模块依赖和兼容行为明确。
- [x] API 所有权、借用期限、并发限制及失败输出有文档。
- [x] 修改代码符合第 1 节适配规则，Doxygen 与头文件契约一致。
- [x] 新增/调整头文件可被 C11 与项目 C++17 测试独立包含。
- [x] 分配、整数运算、输出预算、初始化和清理路径经过检查。
- [x] 系统调用与 pthread 返回值正确处理，无静默丢错。
- [x] 正常与关键错误路径测试通过，竞争用例能控制时序。
- [x] 无新增警告；适用的 Sanitizer 与平台检查通过。
- [x] 没有遗留已知 UB、悬空引用或不明确的资源所有权。
- [x] 未验证假设、环境限制和兼容风险在结果中逐项列明。

存在关键门禁失败时不把实现标为完成。工具或平台不支持的验证应明确记录为
未验证，并约定替代环境或后续验证，不能用忽略告警/禁用测试代替修复。

### 8.4 格式检查与规则落地

已新增 morph `.clang-format`，以 clang-format 23.1.1 验证：Tab/8、100 列、Linux
函数大括号、指针星号靠变量，关闭 include 排序与自动加括号，配置项目 foreach 宏。
只格式化本次修改片段和新增文件；没有导入上游整套配置或重排历史文件。

新增 C 文件和内部头文件执行只读格式检查；既有文件保留无关历史格式。
`git diff --check` 用于补充检查。格式工具不能证明线程安全或所有权正确。
CI 与分支保护未变更，全仓格式迁移仍是独立范围。

## 9. 与前一轮建议相比的收敛

1. 不把“建立四个抽象”直接作为实现目标，先解决具体缺陷，再证明共享模块必要性。
2. 不立即新增完整工具 outcome 类型体系，先统一已有 rc 与 envelope 契约。
3. 不把 fanout 的错误检查升级为线程池建设。
4. 不把普通进程 session API 强行套在长期 MCP 连接上。
5. 不把数据库事务、事件投递和外部副作用混为一种原子性保证。
6. 不把 guardrail 故障自动改为全部阻断，先定义检查错误和兼容策略。
7. 不将构建成功或 45 项测试通过表述为已完成全量、Sanitizer 或扩展路径验证。
8. v2 纳入上游风格指南的适用项，v3 实施项目适配；排版、返回路径、命名和
   文档位置不由上游默认值覆盖。必要结构变化来自生命周期和任务身份修复，
   不是为风格迁移扩大公共接口。

## 10. 已落实的行为决策与剩余边界

- 非协作进程内工具：关闭请求回调后由 owner 跟踪，runtime 返回和 shutdown 前
  必须 join，可能等待较长时间。子进程执行有整体超时和终止/回收；不使用
  pthread_cancel，不声称超时可回滚已经发生的外部副作用。
- guardrail：新增 `on_error = "continue" | "block"`。旧配置默认 continue，同时
  保留负 check_error 和诊断；关键规则可显式 block。检查故障与正常 PASS 可区分。
- MCP stdio：接收缓冲上限为 16 MiB；超限返回 `-EFBIG` 并断开连接，发送部分
  帧后失败也断开，避免下一请求沿用已损坏的传输帧。
- 任务：run_token 保证旧执行者不能覆盖已取消或更新后的状态；单次 claim 使用
  条件更新，状态与通知一起提交。未引入租约/心跳，也不承诺外部副作用 exactly-once。
- 动态工具：新格式把权威 source 嵌入元数据；tool.js 是兼容镜像，元数据为唯一
  提交点。旧格式按需升级，checkpoint/恢复/提升也使用相同快照规则。直接手改
  已升级工具的 tool.js 不会替换已提交源码，应通过管理接口发布新版本。
- API：结构化结果构造函数仍返回构造状态，tool_exec 返回业务状态；此前依赖
  “业务失败仍返回 0”的调用者需要按新契约处理。项目内相关测试已调整；
  注册表、guardrail、任务快照等 C 结构新增成员，消费者须使用同一版头文件重新编译。
- 平台：本次动态验证为 macOS arm64；Linux 的条件变量时钟、进程组、目录同步和
  Android/iOS 共用核心的实际构建尚未在对应平台运行，不能据此宣称跨平台全通过。
- 内存工具：本机 LeakSanitizer 不受支持，Valgrind 不可用；ASan/UBSan 和定向
  TSan 的结果不能替代泄漏工具及其他平台验证，也不是对全部源码的安全证明。

未列入范围的框架建设、外部副作用幂等、平台专项验证不是未完成的 R01–R10
实现项；本次完成的是上述具体缺陷修复及本机可执行门禁。

## 11. 参考

- [MaJerle/c-code-style：固定版本的规则正文](https://github.com/MaJerle/c-code-style/blob/9eb29e4aed5b025a42358dcd1b2e38ad2b9cd614/CODING_RULES.md)
- [上游首要原则：保持现有项目风格](https://github.com/MaJerle/c-code-style/blob/9eb29e4aed5b025a42358dcd1b2e38ad2b9cd614/CODING_RULES.md#the-single-most-important-rule)
- [上游格式配置：用于对照，未经适配不直接采用](https://github.com/MaJerle/c-code-style/blob/9eb29e4aed5b025a42358dcd1b2e38ad2b9cd614/.clang-format)
- [上游头文件模板：用于接口与文档对照](https://github.com/MaJerle/c-code-style/blob/9eb29e4aed5b025a42358dcd1b2e38ad2b9cd614/template.h)
- [项目 Agent Guide](../AGENTS.md)
- [Runtime 架构](runtime-architecture.md)
- [状态所有权](state-ownership.md)
- [异步命令契约](async-exec.md)
- [事件系统](event-system.md)
- [定时任务](scheduled-tasks.md)
- [动态 JS 工具](quickjs-tools.md)

## 12. v3 历史验证记录（本次复核前）

环境：macOS arm64，C11 / C++17；普通和 ASan 构建均启用 FastCGI。结果对应
本次实施版本，历史基线测试不计入下表。

| 验证 | 实际结果 |
| --- | --- |
| 普通完整 CTest | 1839 项，最终 1834 通过、5 环境跳过；含 1 项修复后复跑通过 |
| ASan + UBSan 完整 CTest | 1835 项清单，1830 通过、5 环境跳过、0 失败 |
| 最终代码的 ASan + UBSan 定向回归 | 51/51 通过，包含新增迁移与原子写失败用例 |
| 最终代码的普通定向回归 | Stability、StableTask、DynamicToolsTest 共 51/51 通过 |
| 独立 TSan 构建与并发回归 | 5/5 通过；注册绑定保活、子注册表保活、取消读写、runtime 取消、晚到回调 |
| 全部 src C 编译前端检查 | 218 个源文件，项目严格警告选项另加 -Werror，0 失败 |
| 头文件自包含 | 11 个头文件各按 C11/C++17 严格检查，22/22 通过 |
| 新文件格式与补丁空白 | clang-format 23.1.1 只读检查及 git diff --check 通过 |
| MORPH_RETURN 复查 | src 和 C/C++ 测试中无残留 |

普通完整测试包含两项全局状态审计；ASan 完整测试未启用该审计，所以清单数量
不同。之后新增两项回归均进入最终 51 项定向检查，不把重复运行次数累加成独立
测试覆盖数量。编译警告结论限定于项目 src；首次完整依赖构建中仍有既有的
mathjax/wasm3 deprecated sprintf 警告，没有通过关闭警告掩盖它们。

实际运行方式：

```sh
cmake --build build -j 6
ctest --test-dir build --output-on-failure -j 4
ctest --test-dir build --rerun-failed --output-on-failure
cmake --build build-asan -j 6
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure -j 2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  build-asan/bin/morph-tests \
  --gtest_filter='Stability.*:StableTask.*:DynamicToolsTest.*'
```

TSan 使用单独构建目录，编译/链接添加 `-fsanitize=thread -fno-omit-frame-pointer`，
未与 ASan 混用。执行后移除了本次临时 TSan 构建目录；结果日志保留在 `/tmp`。
C 源严格检查依据 compile_commands.json 对全部 src .c 重放编译参数，加
`-Werror -fsyntax-only`，不是只检查本次增量重新编译的文件。

5 项跳过分别是两个需要 MORPH_TEST_MCP_COMMAND 的 live MCP 测试、需显式启用
的 host automation、宿主文件系统不支持的非法 UTF-8 文件名，以及要求
MALLOC_PERTURB_ 的堆扰动验证；跳过没有计为通过。原子写受限失败使用独立子进程
RLIMIT_FSIZE 注入，确认旧内容及临时文件清理；通知 SQL 错误通过 trigger 注入。
没有对每一次分配、rename、fsync 或掉电场景逐一注入，不能据此声称穷尽所有故障。

本机尝试开启 detect_leaks=1 后，LeakSanitizer 明确报告不支持该平台并在测试前
终止，因此采用上述 detect_leaks=0 完成 ASan/UBSan；Valgrind 未安装。Linux、
Android/iOS、真实外部模型/MCP 服务及完整 TSan 套件尚未在对应环境验证。

普通完整 CTest 原始记录保留 completion 用例修复前的 1 次失败；修复后通过
`ctest --rerun-failed` 复跑，该项 CTest 通过，独立三种布局场景也全部通过。
没有把修复前的失败日志改写为一次性全绿。

测试可靠性修正：composer 和 completion PTY 用例通过共享 helper 等待真实
Readline 输入框就绪后再发送输入，替换固定 1 秒启动假设，保留原有颜色、宽度、
历史留白等断言。失败终端记录证实提前输入发生在欢迎信息之前；没有通过
增加执行超时或禁用断言规避问题。

主要日志：`/tmp/morph-review-ctest-delivery.log`、
`/tmp/morph-review-ctest-delivery-recheck.log`、
`/tmp/morph-review-ctest-asan-complete.log`、
`/tmp/morph-review-asan-delivery-regressions.log`、
`/tmp/morph-review-tsan-delivery.log`、`/tmp/morph-review-strict-final.log`、
`/tmp/morph-review-headers-final.log`。本文保留结果摘要，日志不是仓库交付文件。

## 13. v4 二次复核与完成判定

用户再次要求检查“是否 100% 完成”后，重新核对实现、退出路径、线程同步、
测试矩阵和实际日志。结论是：v3 的“全部闭环”表述过满，本次确实发现并修复了
遗漏，不能仅凭上一轮测试结果维持无条件完成结论。

### 13.1 本次补漏

| 范围 | 复核发现 | 修复与回归 |
| --- | --- | --- |
| R03 | 正常完成后的 pthread_join 返回值被忽略 | join 失败按基础设施错误返回，并交回 owner 跟踪；JoinFailureRetiresWorkerAndReportsInfrastructureFailure |
| R03 | 历史写入等提前退出绕过剩余 worker 清理；超时失败还可能重复转移同一 call | 单一清理出口，join 失败立即清空 slot 所有权；HistoryFailureRetiresEveryOutstandingWorker，SQL trigger 注入及晚到回调断言 |
| R03 | 超时文本没有说明副作用未知 | 增加副作用未知、禁止自动重试提示，保留现有超时码 |
| R04 | 原子替换没有拒绝注册表初始化失败；普通结构变更未参与管理操作同步 | 保留初始化错误及 caller 所有权；register/unregister/replace 统一 mutation→data 锁序，短数据锁不覆盖工具调用 |
| R04 | 审批回调扩容后，创建流程仍保留旧 entry 指针 | 审批/验证后重新取得当前 entry；ApprovalRegistryGrowthCannotInvalidateTheCheckpointSource 在审批回调中注册 128 个工具触发扩容 |
| R04 | MCP 发布与后台结构修改缺少同一锁序；facade 标量读取也未取得注册表数据锁 | 发布按 execution→mutation→facade→data 锁序复制；等待保持可取消；同步注册/发现与 metadata 查询统一协调；PublicationWaitsForMutationWithoutBlockingCatalogQueries |
| R05 | stdout/stderr 关闭并回收 child 后，未发送完的请求可能被当作成功 | stdin 纳入完成条件；child 提前退出且请求未发完返回 -EPIPE；背压填满管道的错误用例 |
| R05 | 多处管道没有 close-on-exec，JS 检查器也未隔离进程组/继承 fd | 共享 morph_io_pipe，Linux 原子 pipe2；macOS spawn 默认关闭非显式继承 fd；检查器建立进程组并关闭继承 fd |
| R03/R06 | 扩大的 TSan 检查发现 vendored cJSON 的 global_error 被主循环和 worker 同时写入 | 将既有 vendor 私有错误游标改为 C11 _Thread_local，并避免 NULL 指针加零；ConcurrentJsonParsingKeepsErrorCursorsThreadLocal 与 TSan 回归 |
| R09 | JS 校验晚于元数据发布，崩溃窗口可能留下未验证版本 | 临时快照先校验，再 checkpoint/发布；InvalidSourceNeverCreatesAPublishedToolDirectory |
| R09 | 源码长度先转换 int 再比较上限 | 用 size_t 检查后再处理，避免大输入绕过上限 |
| R02/R10 | FULLMUTEX 只序列化单次调用，SAVEPOINT 多步提交仍可被同连接其他线程插入；changes() 也可被覆盖 | 连接原生递归互斥量保护短事务及 step/changes/finalize；100 个任务与并发查询的 trace 回归 |

join 失败用例最初通过真实 detach 制造故障，ASan 明确拒绝该非法 join。已改为
实例级内部 join adapter：只让第一次 join 返回 EINVAL，后续用真实 pthread_join
回收 worker，不关闭 Sanitizer 或跳过该错误路径。

关键回归新增 10 项。最初 8 项进入 60 项定向检查，后补 MCP 发布协调用例，
相关 81 项定向检查中 80 项通过、1 项 live MCP 环境跳过。
jobs PTY 的 until helper 也补上同步帧提交检查：标记文本可能先于 draft 重绘到达，
不能在 2026 同步更新尚未结束时断言终端最终状态；原业务和布局断言保留。并发 trace 用例是压力补充，
不把一次通过等同于穷尽所有时序。没有新增全局注入开关或公共线程测试框架。

注册表锁序已写入 tool.h：结构变更先取得 mutation_mutex，再取得短数据锁；
调用者不能只持有原始数据视图锁时调用结构变更。两个锁均允许项目内部同线程
嵌套；普通工具执行仅在取得引用时持短锁，执行和最终析构仍在短锁外。

cJSON 的调整收窄的是已有 vendor 私有解析状态，保持 GetErrorPtr 的同线程 API；
业务层没有新增全局配置、日志上下文或注入开关。分配器 hooks 仍按上游约定只在
启动阶段配置，不支持与解析并发重配置。

### 13.2 本次验证

- 最终普通全量 CTest：1849 项，1844 通过、5 环境跳过、0 失败；204.19 秒。
- 最终 ASan/UBSan 全量 CTest：1847 项，1842 通过、5 环境跳过、0 失败；190.04 秒。关闭本机不支持的 LeakSanitizer。
- 独立 TSan：最终 11/11 通过，覆盖历史失败回收、join 失败、审批扩容、同连接任务提交、MCP 发布协调和 JSON 错误游标隔离。
- 全部 src C：218 个源文件加 -Werror 的编译前端检查通过，0 失败；vendored cJSON 另做严格检查通过。
- 公共/内部头文件：13 个头文件按 C11/C++17 共 26/26 自包含严格检查通过。
- Android NDK 25.1：arm64、API 24，三个共用 util 源文件和 vendored cJSON 严格交叉语法检查通过。
- iOS SDK 27.0：arm64、最低 iOS 15，三个共用 util 源文件和 vendored cJSON 严格交叉语法检查通过。
- git diff --check、新文件 clang-format 只读检查通过；MORPH_RETURN 无实现残留。

移动端检查只针对本次共用 util 源文件和 cJSON，不是移动应用的完整编译、链接或设备运行。
前端锁定的 vendor core 快照没有被替换为未提交工作区。Docker CLI 存在，但本机
Docker daemon socket 不存在，未获得可运行的 Linux 容器环境。

### 13.3 验证尚未穷尽的项目

以下不能勾为通过，也不能从“实现完成”推导为“已全部验证”：

- [ ] Linux 原生构建与进程/信号/条件变量运行验证。
- [ ] Android/iOS 完整应用构建、链接及设备运行。
- [ ] LeakSanitizer 或 Valgrind 泄漏检查；本机前者明确不受支持，后者未安装。
- [ ] 两项 live MCP、host automation、非法 UTF-8 文件名、堆扰动等跳过条件下的专项验证。
- [ ] 第 7 节矩阵中逐个分配点 OOM、rename、目录 fsync、真实磁盘故障/掉电的完整注入。

第 7 节保留为完整覆盖目标，第 12、13 节才是已执行证据。核心修复可以逐项完成，
但验证覆盖尚不能宣称 100%；不再以“平台专项验证不属于实现项”替代这一区分。

本次日志前缀为 `/tmp/morph-recheck-`：构建、定向回归、全量 CTest、TSan、严格编译、
头文件和移动端语法检查分别保留独立记录。

最终两套全量测试均基于包含 cJSON 修复的同一份最终工作区实现，一次完整运行
零失败，不依赖此前的失败项复跑拼接结果。普通套件比 ASan 多两项全局状态审计；
两边的 5 项跳过仍单独记录，不计为通过。最终证据日志：

- `/tmp/morph-recheck-close-ctest.log`
- `/tmp/morph-recheck-close-asan-ctest.log`
- `/tmp/morph-recheck-tsan-json-tests.log`（11 项）
- `/tmp/morph-recheck-final-strict.log`、`/tmp/morph-recheck-cjson-strict.log`
- `/tmp/morph-recheck-final-headers.log`（26 项）
- `/tmp/morph-recheck-ios-final.log`、`/tmp/morph-recheck-android-final.log`

v4 的最终判断：已修复本次复核实际发现的遗漏，已完成上述本机可执行检查；
第 13.3 节的未验证项仍然存在，因此不将整体工作表述为无条件“100% 完成”。
