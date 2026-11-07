# JSON 模块

`util/json.h` 提供 C11 和 C++17 共用的 JSON 接口，链接 `morph-util`。
`json` 是不透明节点；实现使用 cJSON，调用者不访问后端字段。
项目内 C/C++ 调用统一使用本模块；cJSON 类型、字段和错误游标只存在于私有实现。

## 常见用法

```c
#include "util/json.h"
#include <errno.h>

json *j = json_object();
if (!j)
	return -ENOMEM;
int rc = json_set(j, "user.name", "Bob");
if (!rc)
	rc = json_set(j, "user.age", 26);
if (!rc)
	rc = json_append(j, "user.tags", "developer");
if (!rc)
	rc = json_set(j, "user.active", JSON_BOOL(true));
if (!rc)
	rc = json_set(j, "user.note", JSON_NULL);
if (!rc) {
	const char *name = json_str_or(j, "user.name", "unknown");
	int64_t age = json_int_or(j, "user.age", 0);
	/* Borrowed name remains valid until its node is changed or freed. */
	(void)name;
	(void)age;
}
json_free(j);
return rc;
```

C11 使用 `_Generic`，C++17 使用显式 C++ linkage 的薄重载；头文件可以放进
调用者的 `extern "C"` 块。泛型参数支持普通有符号/无符号整数、bool、float、
double、C 字符串和 `json *`/`const json *`；root、path、value 各求值一次。
未知指针等未支持类型在编译期拒绝。

C11 的 `true`/`false` 是整数宏，泛型布尔字面量请用 `JSON_BOOL(true)`，或传入
`bool` 变量。C++ 的 `true` 可直接使用。使用 `JSON_NULL` 表示 JSON null；C++
也支持 `nullptr`。`NULL` 的定义随语言和平台变化，不能可靠地与数字零区分。
空字符串指针和空节点指针是参数错误，不自动变为 JSON null。

## 解析与字符串

`json_parse()` 和带长度的 `json_parse_n()` 严格消费完整输入，只允许末尾 JSON
空白。后者可处理没有 NUL 终止符的输入，并通过 `json_error` 返回本次调用的
负错误码、从零开始的字节偏移和静态诊断文字，不使用全局解析错误状态。
只有显式 `json_parse_prefix()` / `json_parse_ex(..., false, ...)` 接受合法前缀，
其 `end` 指向第一个未消费字节。

普通字符串、对象键和解析输入要求合法 UTF-8。支持 JSON escape 和合法的
UTF-16 surrogate pair；拒绝无配对 surrogate。嵌入 NUL（包括 `\u0000`）
明确不支持；`json_string_n()` 不会把 NUL 后的数据悄悄截掉。
`json_string_bytes_n()` 只供内部 UTF-8 分块数据使用，允许不完整字节片段，仍
拒绝 NUL；包含非法 UTF-8 片段的树不能序列化，返回 NULL、`errno=EILSEQ`。

数字是有限 IEEE double。构造和更新拒绝 NaN/Infinity，解析拒绝溢出及非零数字
下溢到零，允许非零 subnormal。泛型整数只接受
`[-9007199254740991, 9007199254740991]`，超出返回 `-ERANGE`，避免静默丢位。
这不提供任意 64 位整数的精确 JSON 表示；需要更大 ID 时由业务明确使用字符串。
序列化保留有限数值的往返结果，但不承诺原始数字拼写或负零符号。

JavaScript 运行器的 VM 值保留 QuickJS 原生 JSON 语义，包括字符串中的 NUL；
它通过 VM 对象构造和原生序列化响应，不把 VM 值强制转换成 C 字符串节点。

## 路径与读取

路径区分大小写：`user.tags[0]`、`[0].name`、`matrix[0][1]`。
数组下标是无符号十进制，允许前导零。裸 key 中的 `.[]\` 需用反斜杠转义；
也可使用 JSON quoted key，如 `["literal.key"]`、`[""]`、`user["a[b]"]`。
单独的 literal API `json_member()` / `json_object_add()` 等从不解释 key 的路径含义。

NULL 或空 path 表示 root。`json_set()` 可原地替换 root，root 指针保持有效；
`json_append()` 的 root 必须已经是数组；`json_erase()` 不允许删除 root。
缺失分支按后续路径段创建对象或数组，append 的缺失目标创建为数组。
已有中间 null/scalar 不自动改型。下标小于数组长度时 set 替换，等于长度时
追加，大于长度时报错，不用 null 填洞。深度最多 `JSON_MAX_DEPTH=128`，root
深度为 1，scalar leaf 也计一层。

读取和遍历返回借用节点。`json_get()` 缺失或路径非法返回 NULL，`json_exists()`
可区分缺失与实际 JSON null。`json_str/int/num/bool_or()` 在缺失或类型不符时
使用 fallback，现有的空字符串、零和 false 不触发 fallback。
`json_read_*()` 提供带错误码的严格读取，不做字符串/数字/bool 转换；整数读取
拒绝小数、负 unsigned 和实际整数范围外的 double，失败时不改写输出参数。
重复对象键保留顺序；lookup/replace/remove 作用于首个匹配项；CI 变体只做
ASCII 大小写折叠。普通 `_add` 允许重复键，`_replace` 要求目标存在。

## 所有权与失败

构造、parse 和 clone 返回独占节点；`json_free()` 递归释放；释放已挂载节点会
先从父树移除。直接 object/array add/replace 成功才转移节点，失败仍由调用者
拥有；拒绝重复挂载、self/ancestor 环及超深树。detach 返回重新独占的节点。

泛型 set/append 复制字符串、深克隆节点，包括借用节点；不会接管实参节点。
显式 `json_set_take(..., &owned)` / `json_append_take()` 只在成功时释放原 owned
并置空，失败保留它；内部允许克隆来保证事务安全，不承诺转移后的节点身份。
`json_take_string()` 则始终消费 malloc 字符串，包括失败路径。
修改可能使受影响的借用节点或字符串失效；不要缓存它们跨修改使用。
`json_foreach` 不自动保护删除操作；删除当前节点前先保存 `json_next()`，再从已保存
的下一节点继续，不对已释放节点调用遍历函数。

状态函数成功返回 0，失败返回负 errno；构造/序列化返回 NULL 并设置 errno。
常见错误是 `EINVAL`（语法、参数或类型冲突）、`ENOENT`（缺失）、`ERANGE`
（数值/索引范围）、`E2BIG`（深度）、`EILSEQ`（UTF-8）、`ENOTSUP`（嵌入 NUL）
和 `ENOMEM`。修改失败保持原树不变；缺失分支在离树状态构造后一次提交。
`json_stringify*()` 返回的文本用 `json_text_free()` 或 free 释放；arena 变体
由 arena 清理。模块无全局可变状态；跨线程共享同一棵树时由调用方同步。

## 验证

`morph-json-tests` 独立覆盖 C11/C++17 泛型、严格解析、路径、所有权、深度和
固定种子的数值/修改往返回归。`morph-json-oom-tests` 在独立进程重编模块、
容器和后端，逐个分配点注入失败，验证原子性、所有权和释放；故障上下文属于
fixture，通过该测试进程的环境传递，不给生产代码增加 allocator hook/global。
CTest 另有 C/C++ 未知指针的负向编译检查和后端私有边界检查。迁移前先独立通过
模块功能和分配失败测试，再执行完整业务回归。
