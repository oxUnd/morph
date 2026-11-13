# 示例

每个公共模块至少有一个可运行示例。

| 模块 | 示例 |
| --- | --- |
| 总览 | `basic.c` |
| `base` | `base.c` |
| `err` | `err.c` |
| `mem` | `mem.c` |
| `str` | `str.c` |
| `buf` | `buf.c` |
| `vec` | `vec.c` |
| `map` | `map.c` |
| `log` | `log.c` |
| `time` | `time.c` |
| `file` | `file.c` |
| `encoding` | `encoding.c` |
| `ini` | `ini.c` |
| `toml` | `toml.c` |
| `utf8` | `utf8.c` |
| `hash` | `hash.c` |
| `thread` | `thread.c` |
| `socket` | `socket.c` |
| 综合案例 | `case_log_index.c` |

`case_log_index.c` 是一个小型日志索引器：INI 配置、arena/pool 分配、
vec 保序、map 去重与分级统计、buf 序列化、str 视图解析，同时用到
hash/encoding/utf8/time/log/err/file。

构建后可直接运行：

```sh
./build/nbase_example_map
./build/nbase_example_socket
```
