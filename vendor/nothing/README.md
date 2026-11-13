# nbase

`nbase` 是一个小型 C11 基础库，面向 Linux/macOS/Windows，零强制第三方依赖。
它提供可复用的内存、容器、字符串、日志、文件、编码、INI、线程和 socket 设施，
不包含事件循环、HTTP、TLS 或脚本运行时。

当前版本：`0.1.0`（ABI 未冻结）。

## 模块

| 头文件 | 内容 |
| --- | --- |
| `nb/base.h` | 平台宏、版本、断言、常用工具宏 |
| `nb/mem.h` | allocator 接口、arena、固定块 pool |
| `nb/str.h` | 借用的字节切片、比较（含 ASCII 忽略大小写）、查找、切分、数值解析、FNV-1a 哈希 |
| `nb/utf8.h` | 严格 UTF-8 校验、编解码、计数/索引/切片、Unicode 18 空白/宽度/类别、NFC 全量 casefold、净化与字节边界裁剪 |
| `nb/hash.h` | CRC-32、增量/一次性 SHA-256、HMAC-SHA-256、常量时间比较 |
| `nb/buf.h` | 可增长字节缓冲，支持 detach、fd 读取、行切分、截断与整数追加 |
| `nb/vec.h` | 按元素大小存储的动态数组，支持插入与删除 |
| `nb/map.h` | 字符串键哈希表，保留插入顺序；支持旧值回传与预留容量 |
| `nb/err.h` | 可移植状态、OS 诊断、显式领域错误表与无全局 logger 的 trace |
| `nb/log.h` | 分级日志、时间戳、可运行时替换的 sink、线程安全 |
| `nb/time.h` | realtime/monotonic 时间、RFC 3339 本地/UTC 格式化与严格解析 |
| `nb/file.h` | 原子写/流式复制、有界递归删除、目录快照、路径/根包含、临时文件/目录、文件锁、mode/symlink/readlink、pipe 与有 deadline 的 fd 写入 |
| `nb/encoding.h` | Base64（含无分配校验）、hex、URL 编解码 |
| `nb/ini.h` | INI 解析（引号、转义、行内注释）、查询、修改、写回与原子保存 |
| `nb/toml.h` | TOML 1.1 AST 解析：表、数组表、内联表、字符串、整数、浮点、布尔与日期时间 |
| `nb/thread.h` | opaque mutex/cond/thread owner、相对/单调/实时时钟等待、线程 ID、stack/detach 选项、C/C++17 共用 ABI 的原子操作 |
| `nb/cancel.h` | 线程安全取消令牌 |
| `nb/socket.h` | 地址解析、DNS、TCP/UDP、阻塞/超时连接、keepalive 与收发超时 |
| `nb/random.h` | 加密安全随机字节 |
| `nb/poll.h` | fd 就绪等待；Windows 后端仅支持 socket |
| `nb/process.h` | stdio pipe/inherit/null/fd、子进程 PATH、POSIX PTY/会话/prepare、进程组、双向有界交换、deadline 写入、流 owner 转移、等待/终止 |
| `nb/signal.h` | POSIX 显式 signal set listener、owner-thread stop 与原掩码/处理器恢复 |
| `nb/queue.h` | header-only intrusive 双向队列 |
| `nb/cleanup.h` | typed destroy 回调、GCC/Clang 可选 scope-cleanup 与 libc 字符串 owner 转移 |

`nb/nbase.h` 是包含全部公共头的聚合头。

## 近期能力补充

- 容器：`nb_vec_insert/remove`、`nb_buf_truncate/append_i64/append_u64`。
- 容器：`nb_buf_shrink`/`nb_vec_shrink` 回收未使用容量；`nb_arena_checkpoint`
  与 `nb_arena_release` 支持阶段内回滚；`nb_map_reserve` 按 3/4 负载因子预留，
  map entry 使用内部 slab/free-list，频繁增删变长 key 时减少 malloc 碎片。
- 文本：`nb_str_eq_icase`、`nb_str_find_char`、`nb_split_iter`；`nb_map_put`
  回传被替换的旧值，`nb_map_reserve` 预扩容。
- UTF-8：RFC 3629 严格校验、码点编解码/计数/切片、Unicode White_Space trim、
  BOM、净化与不拆分码点的字节裁剪。内置 utf8proc 2.12.0 / Unicode 18.0.0 数据，
  `nb_utf8_width_cp`、类别查询与 `nb_utf8_casefold` 共用该后端；casefold 写入 NFC
  规范化的完整折叠结果。容器保持字节透明，不隐式改写文本。
- 哈希：新增 `nb/hash` 模块（CRC-32、SHA-256、HMAC-SHA-256、常量时间比较）；
  密码存储请使用 Argon2/bcrypt/scrypt，模块不提供 MD5/SHA-1。
- 配置：INI 支持引号、转义与行内注释；`nb_ini_save` 原子写回；
  `nb_base64_valid` 提供无分配校验。
- TOML：新增 `nb/toml` 模块，手写递归下降解析器构建只读 AST，覆盖
  TOML 1.1 的表/点号键/数组表/内联表、四类日期时间、数值与多行字符串；
  官方 `toml-test` 语料作为独立 CTest 运行。
- 文件/时间：`nb_file_write_all_atomic` 支持 mode、create-only、父目录 fsync
  与符号链接拒绝；路径模块提供
  `nb_path_join/absolute/clean/canonical/dirname/split/set_extension`、
  `nb_path_is_within/executable/temp_dir` 与 basename/extension 提取；
  `nb_time_format_rfc3339_utc`；RFC 3339 解析按固定位置严格校验。
- 线程：`nb_mutex_new` / `nb_cond_new` / `nb_thread_create` 创建 opaque owner，
  `nb_thread_join_take` / `nb_thread_detach_take` 只在成功时释放并清空 owner。
  条件等待区分 signal、timeout 与错误；单调和实时时钟的绝对 deadline 是独立接口。
  线程选项保留 stack size / detached，线程 ID 与原子 storage 在严格 C++17 下可用。
- 进程：stdio 可独立选择 pipe、inherit、null 或借用 fd；显式 PATH 查找使用子进程
  环境。POSIX 提供 PTY、新会话、进程组和受约束的 child prepare 回调；Windows
  使用原生进程/Job Object。`nb_process_exchange` 并发写 stdin、排空两路输出，
  保留上限后的字节继续排空，取消/超时会终止并 reap。持久进程 stdin 与一般 fd
  都支持取消和单调 deadline 的有界写；流 fd 可显式转移给外部 reactor。
- 信号：POSIX listener 在 worker 创建前阻塞明确的信号集合，用 sigwait 接收，
  并在 owner-thread stop 时恢复原掩码与 dispositions。Windows 明确返回 unsupported。
- 文件：目录快照 `nb_dir_scan` 有明确过滤与排序；`nb_path_canonical_missing`
  只追加安全的缺失尾部；`nb_file_copy` 固定内存流式发布并可拒绝源符号链接，
  `nb_file_remove_all` 接受显式深度限制。
- 其它运行时原语：`nb/random`、`nb/poll`、`nb_cancel`、`nb_buf_read_fd`、
  `nb_buf_next_line`、socket keepalive/收发超时，以及可替换 log sink 和轮转文件 sink。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

常用选项：

```sh
-DNB_BUILD_SHARED=ON      # 共享库
-DNB_BUILD_TESTS=OFF      # 不构建测试
-DNB_BUILD_EXAMPLES=OFF   # 不构建示例
-DNB_SANITIZE=ON          # ASan + UBSan
-DNB_WERROR=ON            # 警告视为错误
```

安装后提供 `nbase.pc`，可用 `pkg-config --cflags --libs nbase`；静态链接使用
`pkg-config --static --libs nbase` 补齐平台依赖。也支持 CMake 包：

```cmake
find_package(nbase CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE nbase::nbase)
```

性能基准与模糊测试为可选目标，不加入普通 CTest：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DNB_BUILD_BENCHMARKS=ON
cmake --build build-release -j 8
./build-release/nbase_bench

# 需要提供 libFuzzer runtime 的 Clang（部分 AppleClang 安装未附带）。
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DNB_SANITIZE=ON \
  -DNB_BUILD_FUZZERS=ON -DNB_BUILD_TESTS=OFF -DNB_BUILD_EXAMPLES=OFF
cmake --build build-fuzz -j 8
mkdir -p build-fuzz/corpus
./build-fuzz/nbase_fuzz_text -max_total_time=60 -max_len=4096 build-fuzz/corpus
```

基准覆盖流式缓冲、arena 多轮 reset、Base64/hex 和不同 key 长度的 map；
输出耗时、当前存储分配字节数及分配调用次数。分配字节数不包含 allocator 元数据，
也不等于 RSS。
本轮测量方法、结果和回归覆盖见 [`benchmarks/README.md`](benchmarks/README.md)。

## 示例

每个公共模块至少有一个可运行示例，完整列表见
[`examples/README.md`](examples/README.md)。构建后可单独运行：

```sh
./build/nbase_example_map
./build/nbase_example_socket
```

运行 `ctest --test-dir build --output-on-failure` 会同时执行库测试和全部示例。

## 设计原则

- **显式所有权**：`nb_str` 是借用视图；`nb_buf`、`nb_vec`、`nb_map` 拥有自己的存储；
  容器默认不拥有元素，需要时由调用方负责释放。
- **API 约定**：公共 API 的借用、拷贝、创建和转移命名见
  [docs/api-conventions.md](docs/api-conventions.md)。
- **OOM 策略**：`nb_alloc` 等失败时调用 OOM handler，默认终止进程；handler 不得返回，
  返回后库会 `abort()`。`nb_try_alloc`、`nb_try_realloc` 和容器增长接口返回失败，
  便于可恢复路径使用。
- **缓冲区消费**：`nb_buf_consume` 只推进读游标，不搬移剩余数据；需要回收已消费前缀时
  调用 `nb_buf_compact`。`nb_buf_reserve` 在已消费前缀达到容量一半时压缩；
  较小前缀允许几何扩容，避免逐字节消费追加时反复搬移整个缓冲。
  `nb_buf_append` 和 vec 复制操作支持来源指向自身存储。
- **阶段内存**：`nb_arena_checkpoint`/`nb_arena_release` 可以在一个阶段结束时回滚到
  之前的标记，并释放其后的 block；`nb_arena_reset` 则保留所有 block，按顺序供下一阶段复用。
- **失败原子性**：路径构建与整文件读取成功后才替换输出；编码、INI 写出失败时保留
  原有内容与读游标。INI 设置与解析在分配失败时保留原文档；成功解析会使旧借用视图失效。
- **错误处理**：返回 `nb_status`，需要上下文时传入 `nb_err`；不使用异常或全局 errno 协议。
- **线程模型**：容器不内建锁；日志和默认 allocator 的配置需在并发使用前完成。
- **可选 scope cleanup**：库实现显式释放 owner，不依赖编译器 cleanup attribute。
  `NB_CLEANUP` 仅在 GCC/Clang 下自动调用 typed 回调；其它编译器的调用方必须显式清理。
- **平台**：Linux/macOS/Windows 均已实现。平台相关代码集中在
  `src/platform/{posix,win32}/`；Windows 后端使用 UTF-8 ↔ UTF-16 的 Win32 W API、
  `CRITICAL_SECTION`/`CONDITION_VARIABLE`/`_beginthreadex` 和 Winsock2。
- **原子写**：`nb_file_write_all_atomic` 在目标同目录创建临时文件，写入并
  sync 后发布；已有目标保留权限位，新文件使用显式 `new_file_mode` / `mode` 或
  默认 `0644`。`published` 区分发布前失败与已发布后父目录 sync 失败；失败清理临时文件。
- **UTF-8**：核心容器与 `nb_str` 保持字节透明；UTF-8 和通用 Unicode 操作集中在
  `nb/utf8`。casefold 显式生成 NFC 规范化的完整折叠结果；不提供通用 normalization
  选项、grapheme 切分、终端 ANSI 策略或语言 tokenizer。
- **哈希**：`nb/hash` 只提供标准原语；口令存储需要专门的 KDF，不在基础库范围内。

## 错误码与用户扩展

通用操作状态是封闭集合，既有值不要重定义。`nb_status` 通过 `INT_MIN` / `INT_MAX`
ABI 边界覆盖 signed int 范围，使显式领域码在 C/C++ 中都是有效值；边界常量不作为
操作结果返回。`nb_status_from_errno` 将捕获的 OS errno 映射为可移植状态，并可在
`nb_err` 保留原诊断；`EACCES` 是 `NB_ERR_PERMISSION`，`EPERM` 是
`NB_ERR_NOT_PERMITTED`，错误路径的 errno 0 也不会变成成功。业务和领域错误使用
`NB_ERR_USER_BASE` 起的用户区间，并显式编号，保证跨构建、日志和协议中的含义稳定：

```c
enum { MY_ERR_CONFIG = NB_ERR_USER(1), MY_ERR_AUTH = NB_ERR_USER(2) };

static const nb_err_desc my_errors[] = {
    { MY_ERR_CONFIG, "MY_ERR_CONFIG", "config invalid" },
    { MY_ERR_AUTH,   "MY_ERR_AUTH",   "authentication failed" },
};

const char *text = nb_err_str(my_errors, NB_ARRAY_SIZE(my_errors), err.code);
```

`nb_err_lookup()` 在静态表中线性查找，`nb_err_str()` 返回描述文本；两者都是
无状态纯函数，不分配内存，也没有注册顺序或插件卸载问题。未命中时回落到
`nb_status_str()`，用户区间统一返回 `"user error"`。多个模块应各占一段编号，
例如 `NB_ERR_USER(100 + n)`；不要用 `__COUNTER__` 生成码值。

## 暂不包含

事件循环、HTTP 解析、TLS、GC、JSON、grapheme/语言排序、线程池。这些应在核心 API
稳定后按需增加。

## 参考实现

`reference/` 下的 `php-src` 与 `lighttpd` 仅用于设计参考。两者许可证不同
（PHP License 3.01、Zend License 2.00、lighttpd BSD-3-Clause）；本项目不复制
其代码，只借鉴公开的设计思路。

## 许可证

待定。
