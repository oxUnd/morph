# nbase 性能与回归验证

基准实现见 `bench.c`。启用 `NB_BUILD_BENCHMARKS=ON` 后生成
`nbase_bench`，默认构建和 CTest 不执行性能基准。

## 测量方式

本次结果来自 Apple M1 Ultra、macOS arm64、AppleClang 21、Release / `-O3`。
比较基线是本轮修复开始前的完整工作区快照，包含当时已有的未提交修改，并非 Git HEAD。
同一基准源码分别链接修复前后静态库，交替执行五轮，表中耗时取中位数。
原始中位数及存储分配量保存在 `results.csv`。

分配量使用 `nb_allocator` 的 size 参数计数，不包含系统 allocator 的额外开销，
不代表 RSS。map 每次创建会使用不同 seed，短耗时测试也会受系统调度影响；
几个百分点的差异不视为确定的性能提升或回退。这里的倍数仅适用于对应用例。

| 用例 | 修复前 ms | 修复后 ms | 变化 |
| --- | ---: | ---: | --- |
| 65535 字节窗口，逐字节消费追加 20 万次 | 178.912 | 1.701 | 约 105 倍 |
| arena 每轮 100 次 1 KiB 分配并 reset，共 20 轮 | 0.563 | 0.037 | 保留分配从 1949824 降到 130344 字节 |
| Base64 编码 64 KiB，500 次 | 139.481 | 11.495 | 约 12.1 倍 |
| hex 编码 64 KiB，500 次 | 214.800 | 18.031 | 约 11.9 倍 |
| map 一万条 8 字节 key 插入及 20 轮查询 | 2.593 | 2.271 | 分配量从 1451200 降到 958560 字节，约少 34% |
| map 8 字节 key，10 万次删除重插 | 2.538 | 2.421 | 耗时接近，保留 slab 可复用 |
| map 一万条 64 字节 key 插入及 20 轮查询 | 12.391 | 12.474 | 耗时接近 |
| map 64 字节 key，10 万次删除重插 | 11.739 | 12.110 | 耗时接近 |

流式 buf 的改进允许在已消费前缀较小时扩容，换取摊销的搬移成本；上表对应容量
从 65536 增到 131072 字节。需要立即回收前缀时仍可显式 compact/shrink。
编码只在输入引用输出存储时复制输入；普通编码提前检查长度、校验输入并一次预留。
路径、文件读取和 INI 合并采用临时对象提交，失败原子性会增加暂时的内存占用。

## 回归覆盖

`tests/test_regressions.c` 使用始终移动 realloc 的计数 allocator，检查 sized free、
分配失败和最终存活分配数，并覆盖：

- buf 自追加、已消费前缀引用、引用自身的格式串和 `%s` 参数。
- vec 所有原元素与插入位置的组合，以及扩容时自复制。
- arena 20 轮完全相同分配、跨块与嵌套 checkpoint/release。
- buf 2 万步、map 2 万步固定种子的模型对照测试。
- Base64 两种字母表及 padding 组合，hex/URL 的 0 到 1024 字节往返。
- 六种编码操作的自引用输入及逐点 OOM，非法输入时保留输出和读游标。
- INI 设置、合并、写出的逐点 OOM；合并保留已有值中的嵌入 NUL。
- 路径输入引用输出、整文件读取的自引用和失败保留。
- 浮点嵌入 NUL 拒绝，以及 RFC 3339 的 INT64_MIN/MAX 边界。
- 已关闭写端的 socket 发送返回错误，覆盖新建和 accept 的 socket。

Clang 14 + ASan/UBSan + libFuzzer 在 Linux arm64 上运行了 61 秒，
执行 589893 个输入，未发现崩溃或 sanitizer 报错。输入长度上限配置为 4096；
这次运行自空 corpus 演化，不能据此声称遍历了所有长度和语法路径。
`tests/fuzz_text.c` 保留为可持续运行的模糊测试入口。

已验证 macOS 静态库、共享库、ASan/UBSan，Linux GCC 普通及 ASan/UBSan，
以及 Docker + mingw-w64 + Wine 的 Windows 静态库和 DLL；每组 CTest 均包含
库测试与全部 18 个示例。另验证安装后的 CMake 静态/动态链接使用方。
没有降低断言、关闭 sanitizer 或跳过失败测试。未验证原生 Windows/MSVC 或其他架构；
这些结果不构成库在所有输入、并发调度和操作系统故障下无缺陷的证明。

## 文本与 CRC 热点优化（2026-11-12）

新增 `bench_text.c` 和 `compare_text.py`。本轮使用 Apple M1 Pro、macOS arm64、
AppleClang 21、Release / `-O3 -DNDEBUG`，不与上面的 M1 Ultra 数字直接比较。
基线为提交 `2cad93ff52e8c9cb41c66b8d8a66762070bd5656`，优化前后用同一份
`bench_text.c` 链接对应静态库，不使用 LTO。每种尺寸先运行一轮预热，再交替执行
七轮优化前后版本，记录中位数和 CV；每个进程内部先调用一次被测操作预热。

`text-results.csv` 保存汇总，`text-results-raw.csv` 保存包括预热轮的全部原始记录。
覆盖 16 B、64 B、1 KiB、64 KiB、4 MiB 原始数据。计时前分配并复用输出，不计
输入生成和分配；编码/解码检查完整输出，脚本逐用例对比优化前后的 checksum。
吞吐使用十进制 GB/s：编码按原始输入字节，解码按编码后的输入字节计算。
Base64 的 64 KiB 原始数据对应 87384 B 编码输入；有 CRLF 的用例每 76 字符换行。
UTF-8 同时返回错误位置，ASCII 包括字节校验但不涉及 Unicode 规范化。

### 本轮结果

| 64 KiB 原始数据用例 | 优化前 GB/s | 优化后 GB/s | 加速比 |
| --- | ---: | ---: | ---: |
| Base64 解码：随机内容 | 0.102 | 2.010 | 19.78× |
| Base64 解码：递增字节 | 0.293 | 2.057 | 7.02× |
| Base64 解码：每 76 字符 CRLF | 0.102 | 1.065 | 10.48× |
| hex 解码：随机内容 | 0.339 | 1.655 | 4.89× |
| CRC-32：随机内容 | 0.396 | 1.870 | 4.72× |
| UTF-8：ASCII | 3.120 | 62.836 | 20.14× |
| UTF-8：中文 | 1.510 | 1.513 | 1.00× |
| UTF-8：中英混合 | 1.902 | 1.885 | 0.99× |

这组结果显示大块中文/混合文本基本持平。16 B UTF-8 的入口有约 0.4–0.6 ns 的
额外成本，不能声称每一种尺寸都变快。64 KiB 随机 Base64 解码的优化后七轮中有
一次高耗时离群，CV 为 53.13%；表中仍使用全部七轮的中位数，没有删除该样本。
本轮最终测量的全部原始记录都保留；不把这些数字外推为跨平台排名。

### 实现与契约

- Base64 使用标准/URL 两套 256 项只读表，校验按四字符分块；无空白时按 quartet
  解码，含空白的输入走查表标量路径。保留可选 padding、规范尾部位和空白规则。
- hex 使用 256 项只读表。两种解码仍先校验再 reserve/写入，不增加普通输入的
  临时分配；非法输入、输入借用输出、读游标、NUL 和 OOM 的原有保证保持不变。
- CRC-32 使用 slicing-by-8，保留 IEEE CRC-32 的多项式和初末异或。只读表从
  1 KiB 增到 8 KiB；没有初始化锁或运行期分配。用
  `python3 tools/gen_crc32_table.py` 可重新生成，构建不依赖 Python。
- UTF-8 在至少 32 B 的输入上跳过开头连续 ASCII 块，使用有边界检查的 `memcpy`
  读取非对齐字；首个包含非 ASCII 的字内及其后缀沿用原来的严格逐码点校验。
  这一步主要改善全 ASCII 和较长 ASCII 前缀，没有声称加速全部非 ASCII 文本。

### 复现

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DNB_BUILD_BENCHMARKS=ON
cmake --build build-release -j 8
./build-release/nbase_bench_text 65536
```

`nbase_bench_text [原始字节数 [迭代次数]]` 默认原始输入为 64 KiB，默认迭代数按
约 32 MiB 的原始处理量设置，限制在 8 到 1000000 次。输入用固定种子 xorshift32
产生随机字节，另有递增字节、ASCII、中文和中英混合用例。

用修复前的库编译同一份基准后，可以比较两个可执行文件：

```sh
cc -O3 -DNDEBUG -std=c11 -I include benchmarks/bench_text.c \
  /path/to/before/libnbase.a -pthread -o /tmp/nbase-text-before
python3 benchmarks/compare_text.py /tmp/nbase-text-before \
  ./build-release/nbase_bench_text --output /tmp/text-results.csv
```

脚本默认测试全部五种尺寸，可用 `--sizes 65536` 缩小范围，`--rounds` 至少为 3。
以上命令适用于本轮 POSIX 环境；库本身没有引入 SIMD、平台分支或额外依赖。
重复热输入的结果受缓存、调度和频率影响，没有绑核或锁频，不代表冷内存吞吐。
原始记录中的高 CV 项不能据几个百分点的变化判断性能回退。

### 验证

本轮普通构建、ASan/UBSan 构建各 21 项 CTest 全部通过，UBSan 设置
`halt_on_error=1`。新增字节取值、游标/NUL 保护、CRC 位运算 oracle、非对齐视图、
块边界及 UTF-8 错误类型/偏移对照测试，继续执行原有自引用和逐点 OOM 测试。
Homebrew LLVM 23 + ASan/UBSan + libFuzzer 在最终版本运行 21 秒、127586 次输入，
未发现崩溃或 sanitizer 报错；配置输入上限为 4096 B，不能据此声称穷尽输入空间。
系统 AppleClang 21 没有随附 libFuzzer runtime，所以 fuzz 单独使用已有 LLVM。
