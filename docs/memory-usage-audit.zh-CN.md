# 全仓内存使用复查（2026-11-14）

基线：`12f4d8c`。本轮覆盖项目代码的分配、复制、扩容、清空、缓存和析构入口，
再按数据大小、生命周期和调用频率核验高风险路径。下述“覆盖”指全仓检索与重点路径
人工审查，不是每条执行分支都经过运行验证，也不是“已经不存在任何内存浪费”的证明。

## 范围

通过 `git ls-files` 枚举本仓跟踪的 C/C++、Objective-C、TS/TSX、JS、Python、
shell 等代码，排除 vendor。统计包含头文件、测试夹具和示例，不能把行数当运行覆盖率。

| 目录 | 代码文件 | 行数（本轮修改后、基准文件加入前的扫描快照） |
| --- | ---: | ---: |
| src | 721 | 155585 |
| web | 46 | 4039 |
| exts | 14 | 3195 |
| scripts | 5 | 745 |
| eval | 7 | 788 |
| examples / misc / skills | 3 | 190 |
| tests | 177 | 85647 |

`fronts/morph-markdown` 是被本仓忽略的独立 Git 仓库；本轮只阅读其边界与核心缓冲
实现，未修改、未构建或验证它的 Android/iOS SDK。vendor 作为固定依赖审查其
分配器/释放契约，不将第三方实现算成项目源码，也没有重新做依赖源码的完整审计。

## 模块核验与处理

| 模块 | 核验重点与结论 |
| --- | --- |
| agent/history/compression | 脱敏预分配、按 secret 的重复复制、历史视图与压缩输入所有权；修复 M1。保留持久历史/模型消息的独立 owner |
| agent/tools、动态工具 | 复核上一轮的元数据借用和脚本读取优化；返回普通 malloc owner 的接口继续保留 allocator 边界复制 |
| models/llm | 空流预分配、工具参数累积、发布和失败原子性；修复 M2/M3。原生参数不能直接交给普通 free owner |
| http/SSE | session reset、响应缓冲、分片解析与析构；修复 M4。SSE 事件回调返回后复用事件容量，析构释放 |
| mcp / ext / ipc | 行帧、通知过滤、RPC JSON 与子进程输出 owner；MCP stdio 帧有 16 MiB 上限，连接析构释放缓冲。仍有大帧高水位与解析副本的优化余地 |
| media / js_runner | 原图/缩放图/编码数据的并存时段；修复 M5。上一轮 JS 读取与源码释放优化继续保留 |
| tui/render | PNG 解码、RGBA owner、协议编码与缓存淘汰；修复 M6/M7。可见图片和解码器内部工作集仍需内存 |
| CLI | transcript、rich document、跨线程事件队列和图片引用；解码后 encoded 缓冲已释放、相同 asset 有引用复用；事件快照复制是线程边界要求。积压风险见 R1 |
| FastCGI | 有界 stream FIFO、token cache、turn tickets、SSE 事件持久化和关闭排空；不把仍在排队/使用中的对象当作泄漏删除 |
| runtime / config / hooks | 文件解析和合并的输入副本、锁与配置提交；修复 M8。失败返回和授权语义保持原样 |
| db / persistence / sync | SQLite column 借用期、checkpoint 原像、恢复计划、backup 流；保留恢复所需的独立字节 owner 和事务生命周期 |
| exec / sandbox / permission | 有界输出窗口、磁盘 spool、secret pending、授权路径与 AST owner；未降低输出和 no-follow 安全约束 |
| skill / prompt / render / event | YAML/模板/渲染结构与 recorder 释放；解析树与输出 owner 分开。recorder 的全量记录是 API 语义，不擅自裁剪 |
| Web | SSE reader/abort、重连 timer、订阅、Blob URL 与 React state；已存在 abort、clearTimeout、unsubscribe、revokeObjectURL；长会话与 Blob 驻留见 R2 |
| scripts / eval / examples / exts | 一次性输入、评估结果列表与扩展输出检查；未将有限测试数据集合或函数返回的自有字符串误报为泄漏 |

## 已修复的浪费

| 编号 | 改动 | 保持的契约 |
| --- | --- | --- |
| M1 | 历史脱敏初始容量最多 4095 字节，按实际输出增长；注册 secret 不存在时跳过整份复制 | 输出文本、secret 匹配次序、JSON 脱敏规则与原文保留不变 |
| M2 | LLM accumulated/reasoning 懒分配，空流不再预留两个 8192 字节输入容量 | 空串通过 nb_buf_cstr 读取；append 的 sticky error 仍向上传播 |
| M3 | 发布工具调用时直接 steal 已有普通堆身份字符串及外部 arguments，仅复制来自 nb_buf 的 arguments | 先准备所有可能失败的复制，再提交转移；失败不消费 ctx，也不覆盖旧 response；response 的其他字段不变 |
| M4 | HTTP reset 保留常用小容量，body 容量超过 1 MiB、headers 超过 64 KiB 则销毁，在下次追加时重建 | 不限制响应大小，不提前释放当前响应；借用原本就在 reset 失效；小响应继续复用 |
| M5 | 缩放成功后立即释放原图；压缩编码结束后释放所有像素，Base64 完成后释放压缩 sink | 同一编码算法/像素/质量参数；只缩短已用完数据的生命周期 |
| M6 | PNG 解码像素成功后交给私有 take helper，删除整份 RGBA malloc/memcpy | PNG 输入仍为 copy API，输出不依赖输入；RGBA copy API 仍真正复制；asset 记录并调用匹配的 free/stbi_image_free |
| M7 | Sixel 不生成未使用的 PNG/Base64；Kitty/iTerm Base64 由 nb_buf 直接持有，尝试缩容，并按实际 cap 记缓存内存 | 光栅/协议内容、图片 key、可见项保护与缓存重建保持；缩容失败不丢数据 |
| M8 | config 文件校验、model mode 保存、permission policy 保存、hooks JSON 加载直接借用输入 nb_buf | 解析严格程度、错误位置、状态码、原子写入及锁/授权不变 |

## 对照测量

新增可选基准 `tests/bench_memory_usage.c`，默认不构建、不混入功能 CTest。
先在生产改动前用同一基准采样，再在最终实现重建运行。
独立进程在创建任何 nbase owner 前安装统计 allocator，各场景结束均验证 tracked
live bytes 为 0，再恢复 allocator；生产 runtime 不切换全局 allocator。

| 相同负载 | 优化前 | 优化后 | 指标含义 |
| --- | ---: | ---: | --- |
| 16 MiB data URI 脱敏为 21 字节文本 | 33554432 | 4096 | nbase 请求存活字节峰值 |
| 128 个空 LLM stream | 4194304 / 256 次分配 | 0 / 0 次分配 | 内容到达前的 nbase 存活字节/分配数 |
| 16 MiB 响应容量在 HTTP reset 后 | 33554432 | 0 | session body 保留的 nbase 请求字节 |

这些值不含 libc/cJSON/JS 堆、栈或系统内存，不是整个应用 RSS 降幅。
HTTP 原容量为 32 MiB 是几何扩容和尾 NUL 所致；回收阈值不改变最大可读取的响应。

PNG asset 另用 2048×2048 RGBA 图片和 `/usr/bin/time -l` 各测三次：
原实现 RSS [40566784, 40583168, 40583168]，新实现
[40583168, 40583168, 40566784]，中位数相同。对照仅将 media.c 换回基线版本，
其余编译对象、参数和依赖相同，临时构建未改工作区源文件。
因此只确认删掉了一次 16 MiB RGBA 复制/分配，**不声称该场景 RSS 峰值下降**；
解码器内部峰值仍可能主导整个进程的高水位。

复现当前实现：

```sh
cmake --build build --target morph-memory-usage-bench -j 8
build/bin/morph-memory-usage-bench
# 可选：仅测 PNG asset 创建，返回宽高及首字节并在退出前释放 owner
/usr/bin/time -l build/bin/morph-memory-usage-bench /absolute/path/to/fixture.png
```

## 尚需独立设计/负载验证的增长点

- R1：CLI 跨线程 event queue 没有总字节上限；单次 UI drain 的 64 项预算不等于
  队列容量上限。若生产者长期快于 UI，积压会持有完整 JSON/字符串快照。
  这是源码确认的增长风险，未做持续高压的驻留曲线测量；需要保序背压或明确允许
  合并的事件类型，不能直接丢弃工具、权限或完成事件来“节省内存”。
- R2：Web 的 stream items、工具输出和历史 DOM 随会话内容增长；ArtifactCard
  挂载即获取 Blob，active query 可持续持有数据；SSE 未完成块也没有单独字节上限。
  未做浏览器 heap snapshot，不据源码就声称已发生泄漏。分页/虚拟化、按需媒体加载、
  帧限制需单独保证恢复、下载、交互和错误处理语义。
- R3：MCP 连接和个别长生命周期缓冲仍可能保留一次合法大帧的高水位；虽有帧上限，
  多连接时需要累计预算测量。本轮未任意缩小帧限或破坏跨请求剩余数据。
- R4：仍有 nb_buf → 普通 char* 的复制，尤其是 patch/checkpoint、返回字符串的
  config/sync/skill 接口。不能把 detached nbase 内存直接给 free；后续可在逐个
  调用链明确 owner 后改为借用/原生 owner，而不是全仓机械替换。

必要的跨线程快照、可回滚原像、React 不可变 state、cJSON allocator 归一化复制
和仍被引用的媒体不是本轮可直接删除的浪费。上述未实施项没有被标成已解决。

## 验证

- 普通 Release/FastCGI 全目标重建通过，无 warning/error；ASan/UBSan 构建同样启用 FastCGI。
- 最终普通全量：2617 项，2611 项通过、6 项既有条件跳过、0 失败，411.81 s。
- 最后一处仅换行的排版调整后重建通过；模型/权限相关 7 项再跑全部通过。
- 新增回归覆盖小响应复用/大容量回收/下一次追加、普通堆身份字符串转移与原生
  arguments 独立所有权，以及大块内联图片和注册 secret 的精确脱敏结果。
  原有 PNG 像素/key、裁剪、Kitty/iTerm/Sixel、权限模式和配置失败不落盘回归保留。
- source_control_flow_conventions、nbase_usage_conventions、113 模块 CLI layout
  检查和 git diff --check 通过。
- ASan/UBSan 核心 2564 项：2558 项通过、6 项既有条件跳过、0 失败，142.14 s，
  未报告 sanitizer 错误。detect_leaks=0，不称为完整 LSan/Valgrind 检查。

首轮定向 228 项有一个 mock 子进程用例 SIGPIPE，单独原断言重跑通过；
随后完整运行也通过。首轮普通全量有 16 个 PTY 超时，系统 pmset 日志记录同期
多次 Maintenance Sleep（53–218 s），CTest 部分超时时间超过其原定限制。
使用仅随 ctest 进程存活的 caffeinate -i 阻止空闲休眠后，全部原超时测试和完整
套件通过；没有调大超时、放宽断言或改变系统永久电源设置。
Web、独立 fronts 仓库与 Linux/Windows 本轮没有新增运行验证。

最终复现命令（Python 测试依赖沿用既有 venv 配置）：

```sh
cmake --build build -j 8
NO_PROXY=127.0.0.1,localhost,::1 no_proxy=127.0.0.1,localhost,::1 \
  caffeinate -i ctest --test-dir build -j 4 --output-on-failure
cmake --build build-asan -j 8
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  NO_PROXY=127.0.0.1,localhost,::1 no_proxy=127.0.0.1,localhost,::1 \
  caffeinate -i ctest --test-dir build-asan -j 4 \
  -E 'integration|_pty|cli_snapshot|multi_agent_task_evaluation|multi_agent_evaluation_regressions' \
  --output-on-failure
```

caffeinate 只用于本机 macOS 的运行期防休眠，其他平台使用对应的 ctest 命令即可。
