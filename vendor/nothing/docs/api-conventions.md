# nbase API 所有权与命名约定

本文适用于 `include/nb/` 中的公开 API。目标不是统一所有后缀，而是让调用方在不读实现的情况下回答三个问题：

1. 这个返回值是借用还是拥有？
2. 失败时所有权有没有变化？
3. 谁负责释放？

新增 API 必须遵守本文；已有 API 不为了统一命名而做全量重命名。若实现与本文不一致，先补测试和注释，再改实现。

## 1. 基本规则

- 调用方声明 owner 时初始化为 `NULL` 或 `{0}`：

  ```c
  nb_socket *sock = NULL;
  nb_buf buf = {0};
  ```

- 构造函数只在成功时写入输出参数。失败时输出参数保持调用方传入的值，因此调用方应先初始化为 `NULL`/`{0}`。
- 回调使用有类型的函数指针，不使用裸 `void *` 作为回调类型。
- 堆对象拥有自己直接分配/打开的字段；调用方传入的借用值默认不归对象所有，除非 API 明确写了接管。
- 不新增 `*_ex` 扩展函数，也不为已有函数增加等价的 `_borrow`/`_copy` 别名。需要额外能力时优先扩展原函数或原对象。

## 2. 命名速查

| 语义 | 约定 | 现有示例 |
|---|---|---|
| 创建堆 owner | `_new` | `nb_buf_new`、`nb_map_new`、`nb_log_new` |
| 创建文档/解析结果 | `_load`、`_parse`、`_from_*` | `nb_toml_load`、`nb_ini_parse`、`nb_str_from_cstr` |
| 打开 OS 句柄 | `_open` | `nb_socket_open`、`nb_dir_open` |
| 释放堆 owner | `_free` | `nb_map_free`、`nb_socket_free` |
| 销毁嵌入对象 | `_destroy` | `nb_buf_destroy`、`nb_vec_destroy` |
| 关闭 OS 句柄 | `_close` | `nb_dir_close` |
| 借用视图 | `_get`、`_str`、`_cstr`、`_at`、`_root`、`_slot`、迭代器 | `nb_map_get`、`nb_buf_str`、`nb_toml_root` |
| 独立拷贝 | `_strdup`、`_memdup`、`_push_copy` | `nb_arena_strdup`、`nb_arena_memdup` |
| 可失败地转移 owner | `_take` | 新增 API 使用 |
| 无分配地转移 owner | `_steal` | 新增 API 使用 |
| 修改对象 | `_set`、`_put`、`_append`、`_insert`、`_remove`、`_del`、`_clear`、`_reset` | `nb_map_put`、`nb_buf_append` |
| 查询 | `_has`、`_len`、`_size`、`_count`、`_empty`、`_capacity` | `nb_map_has`、`nb_vec_len` |

### 关于 `_create`

当前库用 `_new` 表示新的堆 owner，不引入 `_create` 作为同义写法。只有当领域动词更自然、且 `_new` 明显不合适时才使用 `_create`，并且不能同时提供 `_new` 和 `_create` 两个入口。

### 关于 `_copy` / `_clone`

- 字符串和字节复制沿用 `_strdup`/`_memdup`。
- 对象级深拷贝如果将来需要，使用 `_copy`。
- 当前不定义 `_clone`；不要同时提供 `_copy` 和 `_clone` 两套语义。

### 关于 `_borrow`

现有 API 已经用类型和 `const` 表达借用：

- `nb_str` 永远是借用视图；
- `nb_map_get`、`nb_buf_str`、`nb_toml_root` 等返回借用指针；
- 迭代器返回的 key/value 在 owner 修改前有效。

因此不新增 `nb_buf_borrow`、`nb_map_borrow` 这类平行变体。借用语义写在函数注释里。

## 3. `_take` 与 `_steal`

这两种命名只用于**新 API**，不追溯重命名现有操作。

### `_take`

用于可能失败的所有权转移。失败时源 owner 保持不变；成功时源 owner 被清空。

- 指针 owner 使用 `T **`：

  ```c
  nb_status nb_foo_take(nb_foo **owner, /* ... */);
  ```

  成功返回 `NB_OK` 时 `*owner == NULL`；失败时 `*owner` 不变。

- 值 owner 使用 `T *`：

  ```c
  nb_status nb_buf_take(nb_buf *owner, /* ... */);
  ```

  成功时把 `owner` 重置为 `{0}`；失败时保持原内容。

- `_take` 可以分配内存，因此必须返回 `nb_status` 或 `bool`。不要用返回值直接表示“已取走”。

### `_steal`

用于无分配、不可失败的转移：

```c
nb_foo *nb_foo_steal(nb_foo **owner);
```

成功时返回原指针并把 `*owner` 设为 `NULL`。调用方接管返回值。`_steal` 不分配内存、不调用回调、不执行可能失败的 I/O。

### 现有操作不是 `_steal`

- `nb_buf_move(dst, src)` 是交换两个 handle，两个对象都保持可用，不是 steal。
- `nb_map_del()` 是“按 key 删除并返回值”，保留领域动词更清楚。
- `nb_log_set_default()` 是“安装新 logger 并返回 previous”，不是通用的 take/steal。

## 4. 失败语义

### 构造函数

`_new`、`_open`、`_load`、`_parse`、`_from_*` 这类构造函数：

- 返回指针时，失败返回 `NULL`；
- 通过 `**out` 返回时，失败不写入 `*out`；调用方必须先把局部 owner 初始化为 `NULL`。

现有 `nb_socket_open`、`nb_thread_create`、`nb_socket_accept` 都遵循“只在成功时写 `*out`”。调用方不要把它们当成 `_take` 来传入已有 owner 的地址。

### 替换输出缓冲区

`nb_file_read_all`、`nb_path_join`、`nb_path_absolute`、`nb_path_clean`、
`nb_path_dirname`、`nb_path_set_extension`、`nb_ini_write` 等：

- 成功时替换输出；
- 失败时保留输出内容和游标；
- 输入允许借用输出内部存储。

### 容器修改

- `nb_map_put()` 的 `old_value` 只在成功时有效；失败时 map 不变，调用方不应依赖 `old_value`。
- `nb_ini_set()`、`nb_ini_parse()` 失败时保持原文档不变。
- `nb_ini_parse()` 成功会使之前借用的值、section/key 指针和迭代器失效。

### `_take`

失败保留源 owner；成功清空源 owner。不要实现成“先清空再尝试”。

## 5. 对象字段所有权

| 对象 | 拥有什么 | 不拥有什么 |
|---|---|---|
| `nb_buf` | 自己的字节存储 | 无 |
| `nb_vec` | 元素存储数组 | 元素内部引用的资源 |
| `nb_map` | buckets、entry、key 的副本 | value 默认借用；显式安装 `nb_map_value_dtor` 后才接管仍存在的 value |
| `nb_ini` | section 名、key、value 字符串 | 调用方传入的临时字符串 |
| `nb_toml` | AST/arena 及其中的字符串 | 返回值是借用视图，直到 `nb_toml_free()` |
| `nb_log` | mutex；默认 logger 本身 | sink 的 `ctx` 默认借用 |
| `nb_socket` | 文件描述符 | 调用方传入的 `nb_addr` |
| `nb_dir` | 目录句柄 | 返回的 entry 名在下一次 `nb_dir_next` 前有效 |
| `nb_thread` | 线程句柄/上下文 | 线程函数参数和返回值由调用方定义 |
| `nb_arena` / `nb_pool` | 自己的 block/slab | allocator 对象本身；创建时捕获 allocator 快照，allocator 必须比对象活得久 |

`nb_map` 的 value 和 `nb_vec` 的元素是“借用优先”的设计。不要为了满足“对象拥有所有字段”而强行给每个容器加析构器；需要接管时使用 `nb_map_set_value_dtor`，或由调用方在移除元素后自行释放。

`nb_map_set_value_dtor` 的调用边界：

- 只在 `nb_map_clear()`/`nb_map_free()` 时对当时仍在 map 中的 value 调用；
- `nb_map_del()` 和被替换的旧 value 交还调用方，不调用 dtor；
- dtor 不能对同一个 value 重复释放。

## 6. 回调

- 所有回调参数使用具体函数指针类型，例如 `nb_log_sink`、`nb_map_value_dtor`、`nb_oom_handler`、`nb_thread_fn`。
- 回调的 `ctx` 默认是借用：库不释放它；调用方必须保证它在回调可能被调用期间有效。
- `nb_log_sink` 在 logger mutex 持有期间调用，不能回调同一个 logger，否则可能死锁。
- OOM handler 不得返回；库在其返回后会 `abort()`。

## 7. 线程与全局配置

- 容器不内建锁，调用方负责同步。
- 默认 allocator、OOM handler、默认 logger 的配置应在并发使用前完成。
- `nb_log_set_sink()` 是运行期线程安全的；`nb_log_default()` 的内部创建由平台日志锁保护。
- 默认 logger 的生命周期见下一节。

## 8. 默认 logger 的借用规则

- `nb_log_default()` 返回**借用**指针，库持有该对象；调用方不得 `nb_log_free()` 它。
- `nb_log_set_default(log)` 安装的 `log` 仍归调用方所有，调用方必须保证它在安装期间存活。
- `nb_log_set_default()` 返回的 previous 是**借用视图**：不要因为拿到返回值就释放它。要释放自己安装的 logger，先把它替换掉，确认没有并发使用者后再释放。
- 当前 `nb_log_free()` 不会检测并清空默认 logger；调用方必须遵守上述规则。后续如需加固，应在 `nb_log_free()` 中检测默认指针并同步清理。

## 9. 已知待统一

以下点已经记录，但不在本文档的改动范围内：

- `nb_log_default()` / `nb_log_set_default()` 的 previous 所有权需要在 `nb/log.h` 中写清，并补一个并发/释放测试。
- 构造函数失败时“`*out` 保持不变”目前靠调用方初始化保证；新增构造函数应统一实现和测试这一点。
- `nb_ini_get()`、`nb_ini_iter_next()`、`nb_map_get()`、`nb_map_slot()` 的借用有效期还应补注释。
- `nb_map` 的 value 借用模型和 `nb_vec` 的元素借用模型是刻意设计，不按“对象拥有所有字段”机械改造。

## 10. 参考先例

- GLib/GNOME ownership 指南：`peek` 不转移，`dup`/`take`/`steal` 转移；`get` 的语义依函数而定。
  <https://developer.gnome.org/documentation/guidelines/programming/memory-management.html>
- Fuchsia C API 指南：`create` 和 `take` 表示所有权转移，参数要明确 consumed 还是 borrowed。
  <https://fuchsia.dev/fuchsia-src/development/api/c>
- Apple Core Foundation：`Create`/`Copy` 返回 owned，`Get` 返回 borrowed。
  <https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFDesignConcepts/Articles/NamingConventions.html>
- Python C API：`borrowed reference` 和 `steals a reference` 是文档术语，不是统一函数后缀。
  <https://docs.python.org/3/c-api/refcounting.html>
- Rust RFC 199：`_mut`/`_move`/`_ref` 后缀；Rust 还用 `Borrow`、`Clone`、`Option::take`、`mem::take`。
  <https://rust-lang.github.io/rfcs/0199-ownership-variants.html>

注意：上面没有哪个 C 基础库把 `_borrow`/`_copy`/`_clone`/`_create`/`_take`/`_steal` 这一整套同时作为统一后缀。nbase 采用“操作动词优先、所有权语义显式”的组合，而不是照搬某套后缀。

## 11. 评审清单

- [ ] 每个返回指针都写明 owned 还是 borrowed。
- [ ] 每个 out 参数写明成功时写入、失败时是否清空/保留。
- [ ] 每个 owner 都有匹配的 `_free`/`_destroy`/`_close`。
- [ ] 回调的 `ctx` 生命周期写明，库不默认接管。
- [ ] 容器的 key/value/element 所有权写明。
- [ ] 失败路径不泄漏、不改变可观察状态、不提前清空源 owner。
- [ ] 没有新增 `_ex` 或等价平行别名。
- [ ] 新增 API 有成功、失败和所有权转移测试。
