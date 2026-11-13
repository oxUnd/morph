# nothing / nbase 全量替换评估与计划

- 日期：2026-11-13。
- 状态：源码已下载；初步评估完成；实现迁移尚未开始。
- morph 基线：`df7ca5a4cb4cc9d79713d4de17eff6c576f465f2`。
- nbase 基线：`cd7efa88aa5c3bbd3825d45d3527e45d7566b716`，版本 `0.1.0`。
- 上游：[oxund/nothing](https://codeberg.org/oxund/nothing)。仓库名为 nothing，
  实际库名、API 前缀和 CMake target 分别是 nbase、`nb_`、`nbase::nbase`。
- 本轮产物只有上游源码快照、来源说明和本文；未修改生产代码、测试代码、
  CMake、现有项目规范或 `.gitignore`。

## 1. 评估目标与结论

**终态是移除 `morph-util`，全仓直接采用 nbase 的类型和 API，并收敛散落在各模块
中的重复基础逻辑。** 在 `src/util` 内转调 nbase、保留 `morph_buf_t` 等旧类型，
或者把 util 整体搬到另一个目录，都不算完成。

当前 nbase 已具备多数容器、编码、哈希、文件、线程和进程原语，适合作为替换基础。
但尚不能无差别替换：专项探针已复现 nbase 路径解析的堆越界，文件复制存在整文件
内存加载；Unicode、侵入式队列、C++17 接口，以及进程启动与 I/O 契约存在缺口。
改动还涉及错误模型、资源所有权、日志、TOML、线程同步和构建依赖。
这是一次跨模块重构，不是函数重命名任务。

逻辑层面已经确认多组重复实现：原子写入、文件复制/递归删除、单调时间、流式参数
拼接、行缓冲消费、编码/哈希、子进程管理和路径边界检查。其中部分重复代码的
行为已经分叉。迁移应统一机制，同时明确保留必要的业务规则。

评估方式是全仓静态检索、热点函数逐段阅读、上游独立构建及语义探针。
本文不声称已经逐行审阅全部调用点，也不把上游测试通过等同于 morph 集成通过。

## 2. 已执行的验证

源码导入 `vendor/nothing/`，采用固定提交的完整 `git archive` 快照，共 843 个文件，
与该提交逐文件比较均一致；没有嵌套 `.git`，没有接入 morph 的构建。
`README.morph.md` 是另加的来源说明。

环境：macOS 27.0.1 / arm64，AppleClang 21.0.0，CMake 4.4.4。

| 验证项 | 实测结果 | 说明 |
| --- | --- | --- |
| Debug、`NB_WERROR=ON` | 构建通过，21/21 CTest 通过 | 含库测试、TOML 语料和 19 个示例 |
| ASan/UBSan、`NB_WERROR=ON` | 构建通过，21/21 CTest 通过 | 当前执行路径未报告 sanitizer 错误 |
| TOML 语料 | valid 218/218；invalid 502/502 | 测试检查接受/拒绝，不是 AST 值的完整差分验证 |
| 追加 morph 的 `-Wshadow -Wconversion -Werror` | 静态库构建通过 | 此项关闭了上游测试和示例，只验证库源码 |
| 严格 C++17 公共头 | 19/23 通过，4 个失败 | `thread.h`、`cancel.h`、`cleanup.h`、`nbase.h` 使用/引入 C11 `_Atomic` |
| 严格 C++17 字符串宏 | 失败 | 实际展开 `NB_STR_LIT` / `NB_STR_NULL` 时使用 C99 compound literal |
| 容器/字符串语义探针 | 发现差异 | 见第 4 节 |
| Unicode 标量宽度差分 | 47,035 个标量不同 | 包含未分配码点，不能解读为这么多实际字符都显示错误 |
| 补充长路径 ASan 探针 | 失败，heap-buffer-overflow | 有效输入路径 344 字节，见下文；说明现有测试覆盖存在遗漏 |

### 必须先修复的上游实现问题：realpath 缓冲容量

[`nb_platform_path_canonical`](../vendor/nothing/src/platform/posix/path.c:72)
先分配 256 字节，再调用 `realpath(path, resolved)`。这个调用没有缓冲容量参数，
并不会按这 256 字节截断或可靠地通知“需扩容”；事后检查 ENAMETOOLONG 再翻倍
不能防止第一次写越界。

用仓库外临时目录构造三段各 100 字节的目录名，再调用公共 `nb_path_canonical`，
ASan 复现结果如下。输入路径 344 字节，macOS 解析 `/tmp` 后的规范路径更长：

```text
AddressSanitizer: heap-buffer-overflow
WRITE of size 353
  realpath
  nb_platform_path_canonical path.c:90
  nb_path_canonical path.c:411
  main path-probe.c:10
0 bytes after 256-byte region
  allocated by nb_platform_path_canonical path.c:83
```

**处理要求：** 上游改用平台保证足够容量的方案，或获取系统分配的结果后正确配对
释放；增加长路径、解析失败和借用输出的回归。当前提交不能作为生产路径解析后端。
本轮未修复该问题。最小调用是初始化 `nb_buf/nb_err` 后执行
`nb_path_canonical(&out, long_existing_path, &err)`；探针链接了本次 ASan 版 nbase。

独立构建可以从项目根目录复现；构建目录放在仓库外：

```sh
cmake -S vendor/nothing -B /tmp/morph-nbase-debug \
  -DCMAKE_BUILD_TYPE=Debug -DNB_WERROR=ON
cmake --build /tmp/morph-nbase-debug -j 8
ctest --test-dir /tmp/morph-nbase-debug --output-on-failure

cmake -S vendor/nothing -B /tmp/morph-nbase-asan \
  -DCMAKE_BUILD_TYPE=Debug -DNB_WERROR=ON -DNB_SANITIZE=ON
cmake --build /tmp/morph-nbase-asan -j 8
ctest --test-dir /tmp/morph-nbase-asan --output-on-failure

cmake -S vendor/nothing -B /tmp/morph-nbase-strict \
  -DCMAKE_BUILD_TYPE=Debug -DNB_WERROR=ON \
  '-DCMAKE_C_FLAGS=-Wshadow -Wconversion' \
  -DNB_BUILD_TESTS=OFF -DNB_BUILD_EXAMPLES=OFF
cmake --build /tmp/morph-nbase-strict -j 8
```

尚未执行 Linux、Windows、iOS、Android、Valgrind、集成性能基准和 morph 全量测试。
本轮没有变更 morph 执行路径，不为其声明迁移后的验证结果。

## 3. 重复逻辑审阅：应当删掉什么，统一到哪里

以下结论来自实现逻辑对比，不只依据函数名相似。

### D01：多份原子文件发布实现，持久性和安全语义不同

位置：

- [`file_write_atomic`](../src/util/file.c)：同目录临时文件、完整写入、文件 fsync、
  rename、父目录 fsync；新文件 0600，拒绝符号链接。
- [`cli_management_atomic_write`](../src/sapi/cli/services/management.c:114)：
  同样执行临时文件、写循环、fsync 和 rename，但没有父目录 fsync，使用 stat 读取模式。
- [`atomic_write_config`](../src/agent/tools/config_write.c:211)：
  再实现一遍写循环/临时文件/模式/rename，没有父目录 fsync。
- [`cli_wizard_save_config`](../src/sapi/cli/pages/setup/wizard.c)：
  支持 create-only 的 link 发布、覆盖 rename、父目录 fsync，另外报告是否已经发布。
- [`cli_share_store_share_publish`](../src/sapi/cli/services/share_store.c)：
  临时文件完整写入后 link 为最终文件名，另有失败回收。

**收敛方式：** 这些地方直接使用 `nb_file_write_all_atomic`，由调用方提供 mode、
create-only 等业务选项；删除本地写循环和通用发布实现。
配置编辑的授权、锁、内容冲突检查，setup 的用户选择，share 的文件命名继续由业务负责。

**缺口：** nbase 新文件默认 0644，不能直接用默认选项写敏感配置。
`mode=0` 同时表示“保留已有模式/新建用 0644”；应明确新建模式与保留模式的组合。
setup 依赖“rename 已成功但父目录同步失败”的已发布状态，而 nbase 当前只有返回码，
没有对应状态出参；需要扩展原 API 的结果表达再删除本地实现。
checkpoint 的目录 fd / `openat` / `O_NOFOLLOW` 恢复流程属于更强的目录约束，
不能退化为字符串路径原子写入。

### D02：复制、递归遍历和删除在安装及同步中重复实现

位置：[`ext/install.c`](../src/ext/install.c:331) 的 `copy_file/copy_tree/remove_tree`、
[`skill_store.c`](../src/sapi/cli/services/skill_store.c:138) 的文件/目录复制与递归删除、
[`sync.c`](../src/sync/sync.c:794) 的 `copy_file_plain`、
[`db_backup.c`](../src/sync/db_backup.c:111) 的 `copy_plain`。

同步中的两份 plain copy 都是 fopen/fread/fwrite/flush/close 的同类实现。
安装逻辑也是重复的 read/write、EINTR 处理和目录遍历，但策略不同：
ext 复制保留 symlink、跳过 `.git`；skill 复制要求链接留在源根内，设置递归深度上限，
使用 no-follow/create-only；覆盖、权限、特殊文件处理也不同。

**收敛方式：** 普通复制直接使用 `nb_file_copy`，删除用 `nb_file_remove_all`。
目录遍历机制需要在 nbase 补齐过滤/深度/链接处理的可表达能力，或者由业务遍历
`nb_dir` 并调用 nbase 的单项复制；业务层不能再保留 read/write 复制内核。
包过滤、skill 根约束和事务回滚留在各自业务中。
SQLite 在线备份继续使用 SQLite backup API，不能用普通文件复制代替。

**额外实现缺口：** 所读 POSIX `copy_regular_file` 先 `nb_file_read_all`，再执行
原子写入，把整份源文件装入内存；现有几份 plain copy 使用固定大小缓冲流式复制。
因此不能只因为 nbase 有 copy API 就直接替换大文件同步。需先在 nbase 实现有界内存
的流式复制/临时文件发布，并验证大文件、磁盘满、取消及失败回收。
当前差异是源码确认的复杂度变化；本轮没有测量 RSS 或吞吐量。

### D03：时间读取与 deadline 换算重复，单位和失败处理分叉

位置：[`process_now_ms`](../src/exec/process.c:112)、
[`mcp_stdio_now_ms`](../src/mcp/mcp_stdio.c:27)、
[`runtime_mcp_now_ms`](../src/runtime/mcp_background.c:11)、
[`hook_now_ms`](../src/runtime/hooks_executor.c:142)、
[`cli_jobs_now`](../src/sapi/cli/models/jobs.c:8)、
[`cli_transcript_now`](../src/sapi/cli/services/transcript.c)、
[`morph_io_now_ms`](../src/util/child_io.c:52) 等。

这些实现反复读取 `CLOCK_MONOTONIC` 再转换成整数毫秒或浮点秒；有的检查失败，
有的不检查，返回类型也有 signed/unsigned 差异。各等待循环还重复计算剩余时间。

**收敛方式：** 直接读取 `nb_time_monotonic_ns`，deadline 统一采用明确单位；
展示层在格式化时换算，不能误用读取墙钟的 `nb_time_now_ms`。
deadline 的饱和相加、剩余超时裁剪若反复需要，应补进 nbase 的时间原语。
业务仍决定超时预算、轮询节奏、kill grace 和状态转换。

### D04：模型流式参数有两套相同的扩容与追加算法

[`chat_codec.c`](../src/models/llm/chat_codec.c:195) 和
[`responses_append_call_input`](../src/models/llm/responses_codec.c:146)
都在每个 delta 上执行 strlen、加法溢出检查、realloc、memcpy、NUL 终止。
[`stream.c`](../src/models/llm/stream.c) 还手动增长 tool-call 数组。

**收敛方式：** tool-call 的累积 arguments 使用原生 `nb_buf`，列表使用 `nb_vec`，
两种 provider codec 使用同一领域对象的追加操作或直接追加到对象的 nbase 字段。
删除两份字符串增长算法，避免按片段反复扫描完整前缀。
provider 的 item-id/call-id 路由规则仍各自负责；不能为了合并缓冲而合并协议状态机。

### D05：流式输入反复“找换行、复制行、移动余下内容”

[`mcp_stdio_recv`](../src/mcp/mcp_stdio.c:35) 每读一行后 memmove 剩余缓冲；
[`http/sse.c`](../src/http/sse.c:100) 自行维护未完成行并移动尾部；
[`exec/process.c`](../src/exec/process.c) 也有 pending/tail 的手动压缩逻辑。

**收敛方式：** 使用 `nb_buf` 的读游标、`nb_buf_next_line`、consume/compact，
在消费时减少重复搬移。SSE 的空行分派、多条 data 合并、id/retry、CRLF 规则，
MCP 的请求匹配与最大帧限制，exec 的有界尾部保留策略都继续属于各自协议。
借用行在 append/compact 后可能失效，回调保存数据时必须明确复制。

### D06：编码、摘要与比较不仅在 util 内重写

- [`fastcgi/security.c`](../src/sapi/fastcgi/security.c) 另有 Base64 decoder、HMAC-SHA256、
  hex 编解码和常量时间比较；util 中又有 Base64 和 SHA-256。
- [`runtime/hooks.c`](../src/runtime/hooks.c:29) 和
  [`cli/commands/hooks.c`](../src/sapi/cli/commands/hooks.c) 都把工作目录做 SHA-256 再手写 hex。
- [`sync/content_hash.c`](../src/sync/content_hash.c:8)、`util/id.c`、`skill_store.c`、
  `runtime/session_import.c` 等也手写相同的逐字节 hex 编码。
- [`mcp_http.c`](../src/mcp/mcp_http.c:92) 和 `render/video.c` 都有 percent 编码，
  后者处理 file URL 路径时额外保留 `/`。

**收敛方式：** 直接使用 `nb_base64_*`、`nb_sha256_*`、`nb_hmac_sha256`、
`nb_hex_*`、`nb_hash_equal` 和 `nb_url_encode`，删除局部算法。
hooks 的 review-token 算法由 runtime 提供一个有业务含义的查询入口，CLI 不再重算。
文件 URL 按路径分段编码并保留分隔符，不能把 query encoder 原样套在整条路径上。

**行为边界：** nbase Base64 接受空白和无 padding，现有 decoder 的接受集合不同，
不能意外放宽认证输入；认证格式应保留显式校验规则，并列出有意的校验收紧。
nbase 不提供 PBKDF2，密码格式/迭代策略继续由认证模块负责，内部调用 nbase HMAC。
同步的 BLAKE3 内容标识继续使用 BLAKE3，不替换成 SHA-256 改变既有数据协议。

### D07：子进程启动、等待、管道和取消逻辑多头维护

位置：`util/child_io.c`、`exec/process.c`、`mcp/mcp_stdio.c`、`ext/ext.c`、
`ext/install.c` 的 `run_cmd/wait_child`、`cli/services/management.c` 的
`cli_management_run`、`render/video.c`、`cli/adapters/clipboard/clipboard.c`、
`agent/tools/dynamic_tool_runner.c` 等。

已读实现反复执行 pipe/fork/dup2/关闭端点、exec、EINTR waitpid、退出状态解释。
长期 session、有限 exchange、交互 PTY 的生命周期不同，部分工具还要求受限环境、
进程组终止、超时后排空输出、SIGPIPE 隔离和子进程信号恢复。

**收敛方式：** 通用启动与 I/O 机制采用 nbase；`morph-exec` 保留 session/job、
输出脱敏/落盘、事件和 runtime 集成等领域职责。
不能把当前 `child_io.c` 原封不动搬到 exec 后声称去重完成。

**nbase 当前缺口：** spawn 不搜索 PATH；标准流固定为管道，缺少继承/null/重定向
配置；公开接口无取消式双向 exchange；stdin_write 是阻塞完整写入；没有 PTY、
sandbox/pre-exec 配置入口。所读 POSIX child_setup 没有恢复 morph 屏蔽的信号集合。
这些能力需在上游原语或明确的平台接口补齐；回调必须约束 fork 后可调用内容。
只替换普通进程而永久留几套受限进程底层实现，不满足这次收敛目标。

### D08：路径包含判定重复，已存在与尚不存在路径的语义不一致

[`path_within_root`](../src/sapi/fastcgi/handlers/artifacts.c:27) 和
[`relative_under_root`](../src/sapi/fastcgi/presentation/media_refs.c:61)
都执行两次 realpath，再做前缀比较和 `/` 边界判断；`util/file.c` 又实现一份
包含判断，且对不存在路径尝试解析父目录。
[`scope_path_copy`](../src/agent/agent_control.c) 则逐级寻找最近存在的祖先。

**收敛方式：** 已存在路径使用 nbase canonical/within；新建目标的安全解析需要
nbase 补齐“已存在祖先 + 未创建尾部”的显式契约，不能用 lexical clean 冒充解析。
authorization 的 allow/deny、grant、工作区边界等决策保留在 permission。
checkpoint 的 fd-relative 无跟随遍历不能替换成 realpath 后再 open 的弱化流程。

另一个逻辑重复是 `permission/context.c:path_is_default_temporary` 与
`sandbox/sandbox.c:sandbox_resolve_custom_temp`：都校验 TMPDIR 是已存在绝对目录，
不是 `/`，且不会包含 HOME。nbase 提供路径机制；共享的“可信临时目录”策略应由
permission 统一产出给 sandbox 消费，避免两套许可边界持续分叉。

### D09：数组删除、入队出队也在调用方手工操作存储

`persistence/checkpoint_store.c`、`tui/render/native_media.c`、`tui/core/view.c`、
`cli/services/command_job.c` 等通过 memmove 调整数组并更新元素计数；
`exec/process.c`、`runtime/sync.c` 还自己维护数组容量与 realloc。

**收敛方式：** 由 `nb_vec_insert/remove/reserve` 管理存储变化，业务对象先释放被移除
元素拥有的资源；不把所有 memmove 都当成重复，编辑器的文本区间编辑仍是领域算法。
agent-control/event queue 的侵入式队列需要 nbase 提供对应原语；不能用简单 vector
代替后悄悄改变稳定地址、复杂度和持锁期间行为。

## 4. nbase 还差什么，以及现有契约怎样改变

| 项目 | 当前差异/缺口 | 全量迁移需要的处理 |
| --- | --- | --- |
| 缓冲区 move | morph 清理目标并清空源；nbase 交换两边 | 所有调用点按真正需要的 transfer/swap 改写，不保留旧包装 |
| 空缓冲区 detach | 已分配的空 morph buffer 可返回空字符串；nbase 返回 NULL | 调用方区别空内容和失败；所有权结果优先直接持有 `nb_buf` |
| 缓冲区错误 | morph 是负错误码 sticky 状态；nbase 是 bool sticky 状态 | 全仓改检查方式和返回值语义，不能机械替换 `if (rc)` |
| 字符串比较 | morph 优先比较长度；nbase 是字典序 | 探针 `z` 对 `aa` 分别为 -1/+1；逐个确认排序/查找意图 |
| 结构体布局 | str 的字段顺序/名称不同，buf 增加 off，map 改成不透明指针 | 修改字段、初始化、持有方式和公开头；禁止 typedef/布局强转兼容 |
| 数组初始化 | nbase push 返回未初始化元素；morph 头声称 zeroed，但所读实现没有清零 | 按构造逻辑审计每个新 slot；不把旧文档当作真实保证 |
| map 所有权 | nbase 可安装 value dtor，del/replaced 仍交给调用方 | 明确唯一所有者，禁止调用方和 dtor 重复释放 |
| cleanup | nbase 有 `NB_CLEANUP`，不支持的编译器会静默空展开 | 保留强制编译器能力检查；补齐实际需要的 typed cleanup/steal 原语 |
| 分配/释放 | nbase 按 `nb_free(ptr,size)` 配对，detached buffer 还需 cap | 改拥有类型和释放路径；不能将 nb storage 当作任意 malloc 字符串转交 |
| OOM | `nb_alloc` 默认 abort，`nb_try_*` 和所读容器可返回失败 | 可恢复执行路径使用 fallible API，不引入进程级 OOM 策略修改 |
| 错误模型 | morph 是负 errno/领域码；`NB_ERR_INVAL=-1` 等不对应 errno | 通用错误采用 `nb_status/nb_err`，领域码按 `NB_ERR_USER` 分段；协议旧码只在协议边界转换 |
| 取消状态码 | 有 `nb_cancel`，但当前 `nb_status` 无 cancelled 状态 | 为取消式进程/I/O 明确基础状态，不能把取消混同 timeout 或普通 I/O 失败 |
| 线程/锁/条件变量 | morph 多处内嵌 pthread 对象；nbase 用不透明指针，cond timedwait 以 bool 合并超时/错误 | 改每个 owner 的创建/销毁/失败回收，补齐必须区分的等待结果；不是替换 pthread 符号即可 |
| Unicode | nbase 缺少当前所需 category/casefold，width 数据不同 | 先补 nbase 的完整 Unicode 能力，再删除 morph Unicode 基础实现 |
| 侵入式队列 | nbase 没有当前 queue 的等价模块 | 在 nbase 补原语，替换所有队列节点、宏和操作 |
| C++17 | `_Atomic` 类型和 C99 literal 宏严格检查失败 | 修公共头与 C/C++ ABI 契约，新增真实类型/宏用法编译验证 |
| 日志 | nbase 无当前文件轮转；默认 sink 是 stderr，记录有长度限制 | 上游补文件 sink/长度能力，或 runtime 拥有文件 sink；全仓直接 `nb_log_*` |
| 文件/路径 | 原子写发布状态、私有新文件模式、缺失路径解析及目录策略不足 | 见 D01/D02/D08；在上游补机制，业务提供明确策略 |
| 长路径安全 | `realpath` 写入 256 字节分配块，已复现堆越界 | 先修上游，再接入所有 canonical/within 使用点 |
| 文件复制内存 | POSIX copy 把整个文件装入内存 | 先补流式复制/发布机制，避免替换后大文件同步的内存退化 |
| 进程 | 缺取消式 exchange、标准流选项、PTY、子进程隔离入口等 | 见 D07，补齐后再收敛受限执行路径 |
| TOML | nb_toml 是只读 AST，返回借用的有长度字符串；现有代码访问 tomlc17 具体布局 | 重写 config/schema/edit、ext manifest、skill 等所有消费者及借用生命周期 |

Unicode 差分方法：遍历 `0..0x10FFFF`，跳过 surrogate，比较现有
`utf8_codepoint_width` 与 `nb_utf8_width_cp`，共 1,112,064 个标量。
除未分配码点外，实测 `U+0897` 宽度为 morph 0 / nbase 1，
`U+1FAE9`、`U+1FAEA` 为 morph 2 / nbase 1。不能直接接管终端布局。
建议在 nbase 补齐版本明确的 Unicode 数据或可选 utf8proc 后端及 category/casefold
接口；TUI 只保留 ANSI 解析、终端净化、裁剪等领域策略，并直接使用 `nb_utf8_*`。

通用错误模型迁移也必须覆盖原来直接返回/比较 `-errno` 的实现，不只搜索
`morph_err_t`。系统诊断保存于 `nb_err.os_errno`；原有网络/协议/数据库状态需要
明确映射。日志 null 禁用、实例隔离、文件输出和 shutdown 顺序必须保持。
不采用 nbase 的默认 logger 单例，不在运行中切换全局 allocator。

替换范围还包括散落在 util 外的通用平台原语。当前 `src` 中检索到 71 个使用
pthread thread/mutex/cond 类型或操作的文件；其 owner 要直接改用 `nb_thread`、
`nb_mutex`、`nb_cond`，而不是留一套应用自建线程基础层。
事件循环仍使用 libuv，信号订阅和隔离代码按平台职责处理。
条件变量必须保留谓词循环和 stop/join 顺序；heap handle 的分配失败现在也是初始化
失败路径。`nb_poll` 的超时返回码与 POSIX poll 的 0 不同，也必须重写调用点判断。

## 5. 现有 util 文件的最终去向

| 当前文件/职责 | 替换或归属 |
| --- | --- |
| `buf.*`、`array.*`、`str.*`、`strmap.*` | 全部调用方改用 `nb_buf/nb_vec/nb_str/nb_map`，删除旧模块 |
| `queue.h`、`cleanup.h` | 使用补齐后的 nbase queue 和 `NB_CLEANUP`；领域对象保留自己的 typed cleanup |
| `error.*` | 使用 nbase 错误框架；领域描述表归所属模块；删除旧通用错误框架 |
| `log.*` | `nb_log`；文件 sink/轮转由上游或 runtime 的具体输出服务拥有，不建立兼容日志层 |
| `file.*` | `nb_file/nb_path/nb_dir`，含补齐的通用能力；业务不再保存复制/发布内核 |
| `base64.*`、`sha256.*` | nbase encoding/hash；媒体大小限制和前缀属于媒体领域 |
| `cancel.*` | `nb_cancel`；继续遵守 reset 的独占时机 |
| `id.*` | nbase random/hex；ID 前缀和长度由 session/task 等具体对象的创建过程决定 |
| `name.*` | ASCII 字符验证原语补进 nbase；名称长度、前缀、环境变量规则由具体领域使用原语表达 |
| `child_io.*` | 通用进程交换机制补进 nbase；session 管理由 exec 负责，删除旧通用 I/O 实现 |
| `signals.*` | 跨平台信号机制补入 nbase 平台模块；frontend 负责订阅/关闭，spawn 负责 child reset |
| `terminal_size.*` | TUI terminal 的终端能力；使用 nbase 通用类型和错误，不创建新 util |
| `terminal_status.h`、`spin.*` | TUI terminal/presentation 的状态与动画能力；保持共享符号与 CLI 规则 |
| `utf8.*` | 通用 Unicode 补进 nbase；终端策略到 TUI、tokenizer 策略到 agent；删除旧统一包装头 |
| `bpe.*` | agent tokenizer 的领域模块，内部容器/编码改用 nbase |
| `bash_parse.*` | 命令分析领域模块；建议由 permission 持有中立的分析接口供 exec tool 调用，避免 permission → exec → sandbox → permission 的依赖环 |
| `image_util.*` | 模型、render、TUI 共同使用的媒体领域模块，直接使用 nbase；stb 实现只编译一次 |
| `data.*` | 应用资源定位服务；通用 executable/path 操作使用 nbase，morph 安装布局由该服务负责 |
| `json.*` | nbase 无 JSON；cJSON 变成独立第三方 target，调用方直接用其 API，各具体 owner 负责其 cleanup，删除 morph 通用 JSON 包装 |

这张表覆盖当前 util 的实现职责，不是将文件逐个换目录。
凡属于通用机制且 nbase 尚未覆盖的，先补到 nbase；保留在 morph 的必须有独立领域
含义、实际 owner 和清楚的依赖边界。不得新建 `morph-base/morph-common` 接回全部旧职责。

`cJSON`、stb、SQLite、libcurl、libuv、QuickJS、BLAKE3、tree-sitter 等不是 nbase
现有能力的替身，继续作为独立专用依赖；移除它们对 `morph-util` 的传递链接依赖。
TOML 已有 nbase 对应能力，迁移完成后删除 `morph-toml` 与 `vendor/tomlc17.*`，
同时处理许可证文件的归属。utf8proc 是否继续作为 nbase 后端按 Unicode 方案决定。

## 6. 改动范围与构建影响

以下为基线下 Git 跟踪的 C/C++/Objective-C 源码和头文件的静态标识符统计，
生产统计排除 `src/util` 本身，包含类型/声明，文件数不能相加，且不代表独立逻辑数。
这些数字用于估计触及范围；第 3 节的手写实现不一定含这些标识符。

| 类别 | 生产文件 | 测试/扩展/示例文件 |
| --- | ---: | ---: |
| buffer | 229 | 29 |
| array | 124 | 16 |
| map | 17 | 6 |
| queue | 4 | 1 |
| cleanup 及字符串 owner helper | 210 | 65 |
| 错误框架 | 185 | 46 |
| 日志 | 125 | 2 |
| UTF-8 | 105 | 28 |
| 取消令牌 | 29 | 13 |
| TOML | 9 | 1 |

另外，`util/` include 形式出现在 382 个生产文件、总计 478 个跟踪源码文件中；
25 个 CMake 文件涉及 `morph-util`。仅修改 `src/util/CMakeLists.txt` 无法完成迁移。

需要同步处理：

1. 根 CMake、`src/CMakeLists.txt` 及各模块链接关系：引入一个 nbase 静态目标；
   模块直接声明所用依赖；最终删除 `add_subdirectory(util)` 和 `morph-util` target。
2. `src/util/sources.list` 的生产者与消费者：删除旧清单，领域代码进入对应模块清单。
   `src/tui/samples/CMakeLists.txt` 目前会自行编译 util 子集，必须改成真正的独立依赖图。
3. cJSON、regex、Unicode 等过去夹在 util 中编译的第三方代码拆成清楚的目标；
   tree-sitter 从全局 util 传递依赖改到命令分析模块。
4. nbase 的 `NB_BUILD_TESTS/EXAMPLES` 默认 ON：嵌入生产构建时显式控制，测试仍有
   独立 CI 入口；不引入构建期 git 下载，不依赖浮动 HEAD。
5. 根 `.gitignore` 的 `*.cmake.in` 当前忽略上游必需的
   `vendor/nothing/cmake/nbaseConfig.cmake.in`。本地文件完整，提交依赖时需加窄范围例外。
6. 测试驱动、独立工具、扩展示例及其构建脚本都要迁移，不能只保证主 morph 链接。
7. 本机 `fronts/android`、`fronts/ios` 的构建文件也引用旧 util，但 `fronts` 未被本仓库
   Git 跟踪。实施时要确认它们的交付归属并实际验证，不能把当前主仓变更当作移动端已完成。
8. `AGENTS.md`、`docs/state-ownership.md`、`docs/unicode.md`、TUI 架构与依赖文档，
   以及控制流/全局状态/CLI 结构审计脚本同步更新。旧 UTF-8/错误/容器规范不能与新方案并存。

## 7. 分阶段实施计划

| 阶段 | 工作内容 | 完成标准 | 当前状态 |
| --- | --- | --- | --- |
| P0：取得源码与评估 | 固定快照、独立测试、逻辑重复和契约盘点 | 交付本文与来源记录 | 已完成本轮初评 |
| P1：补齐 nbase 前置能力 | 明确库许可证；修长路径越界；补流式复制、C++17、queue、Unicode、cleanup、文件/路径/进程缺口 | 上游或有记录的 vendor patch 通过行为测试，逐项关闭缺口 | 未开始 |
| P2：建立构建与领域边界 | 接入静态目标，拆分 JSON/媒体/命令分析/资源定位/终端职责，更新规范 | 无新大杂烩库，无依赖环；独立样例可构建 | 未开始 |
| P3：原生类型全仓替换 | 原子迁移一组共享类型及其所有消费者：buf/vec/str/map/queue/cleanup；重写字段与 owner | 该组所有旧类型/API 清零，测试行为成立；不留长期兼容别名 | 未开始 |
| P4：按重复逻辑族收敛 | D01–D09 逐组替换本地算法，覆盖 error/log/cancel/thread/TOML/Unicode/进程 | 每组既有 nbase 调用，又确实删除重复实现；保留策略有说明 | 未开始 |
| P5：删除旧库与验收 | 删除 util 目录、target、清单、包装与旧依赖，更新文档/审计 | 第 8 节全部满足 | 未开始 |

P1 的文件/进程扩展可以按后续逻辑族分别落地，但某族的缺口没补齐，就不能把该族
标为完成。跨模块共有头/类型迁移应按可构建的闭环提交，不能只改库内部。
P2/P3 期间可以存在尚未迁完的旧代码，但最终不保留永久双栈或兼容层。

建议先用高收益且行为可界定的 D03/D04/D06 验证原生 API 迁移方式，接着完成
D01/D02/D05/D09，最后处理 D07/D08 的隔离和权限敏感路径。
这只是实施顺序；所有逻辑族都在最终范围内。

## 8. 全量完成的验收标准

1. 不存在 `morph-util` target、产物、链接依赖；`src/util` 目录及其源清单删除。
2. 生产、测试、扩展、样例不再依赖旧 `util/` include、旧基础类型、API、别名或转调包装。
   领域 API 可以保留 morph 名称，但不能是一层同义基础库 facade。
3. D01–D09 有逐项结果：删除了哪些重复实现、机制由谁提供、保留了哪些领域策略。
   除符号检索外，还复查 read/write/fsync/rename、realloc、编解码循环、时间换算、
   fork/poll/wait 和路径前缀处理，防止只换名字留下重复算法。
4. 所有权与失败路径验证：空输出、分配失败、partial init、move/swap、借用失效、
   容器元素析构、跨线程转移、`nb_free` 和第三方 free 配对。
5. 关键逻辑回归：长路径解析的 ASan 复现消除；原子写模式与已发布状态、create-only、symlink、部分失败；目录过滤
   与根约束；不存在路径授权；Unicode 宽度/category/casefold；Base64 非法输入；
   TOML AST 值/日期/嵌入 NUL 与 config edit/schema 行为；既有密码/摘要数据兼容。
6. 并发/进程回归：线程/锁初始化失败、条件等待与取消、stop/join；大输入同时有 stdout/stderr、输出上限后继续排空、EOF、取消、超时、
   后代终止与 reap、标准流配置、PATH、信号恢复、PTY 和 sandbox；session/job 事件一致。
7. Ask/Auto/Full/Read Only 及不可用审批适配器的权限回归通过；拒绝操作无文件副作用，
   不能因路径解析替换扩大授权。checkpoint 外部修改冲突检查继续有效。
8. 完整构建、CTest、ASan/UBSan、严格告警与 C++17 验证通过；运行
   `source_control_flow_conventions`、`cli_application_layout_conventions`、全局状态审计，
   以及相关 PTY 回归。CLI 验证 120/80/60 列、草稿、流式输出与状态符号。
9. 独立 TUI 样例、FastCGI、readline fallback、所支持平台与移动构建有实际结果记录。
   不把 macOS 单平台上游测试结果扩写成全平台通过。
10. 对流式参数拼接、SSE/MCP 行缓冲、目录复制和长生命周期容器测量时间/分配/峰值内存；
    没有基准前不宣称性能提升。完成后记录最终依赖图与剩余专用平台代码的理由。

## 9. 当前待解决事项

- 当前上游没有 nbase 自身许可证文件；仅找到 `tests/toml-test/LICENSE`，其授权不能
  推定覆盖主库。需要在正式依赖交付前明确并记录主库授权。
- 已确认长路径堆越界、整文件复制内存开销变化；不能用上游 CTest 通过掩盖这些实现缺口。
- ABI 尚未冻结且近期接口变化较多；固定提交升级必须重做相关契约验证。
- 第 4 节所列缺口仍全部存在，本轮没有补上游 API 或隐藏兼容问题。
- 全仓更深的协议/生命周期去重仍需随实施逐族复查；不能用本文热点清单替代最终审阅。

本轮止于评估，接下来应先审阅上述缺口、最终去向和阶段边界，再开展实现。
