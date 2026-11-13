# nbase 开发约定

## 语言与风格

- C11；公共头文件在 `include/nb/`，实现在 `src/`。
- 4 空格缩进，不用 tab；不要使用 `goto`。
- 公共 API 必须带 `NB_EXPORT`；内部辅助函数保持 `static`。
- 注释解释约束、所有权和不变量，不写逐行旁白。

## API 扩展规则

- **禁止新增 `*_ex` 扩展函数。** 不要用 `foo_ex()`、`foo_cstr_ex()` 这类并行变体扩展已有 API。
- 需要额外能力时，优先扩展原函数或原对象：
  - 增加可选出参（例如 `old_value`），调用方传 `NULL` 即可保持简单用法。
  - 在对象上增加 setter 或可选字段（例如 `nb_map_set_value_dtor`）。
  - 增加一个语义独立、命名自然的新操作；不要只是给旧函数加后缀。
- 修改公共函数签名时，必须同步更新所有调用方、测试、示例和文档；项目尚未冻结 ABI。

## 所有权与内存

- `nb_str` 是借用视图，永不释放；需要拥有的字符串使用容器或 arena。
- `nb_buf`、`nb_vec`、`nb_map` 拥有自己的存储；默认不拥有元素/值。
- `nb_map` 只有显式安装 value dtor 后才在 `clear/free` 时销毁仍存在 的值；`del` 和被替换的旧值交还调用方。
- `nb_arena`/`nb_pool` 捕获创建时的 allocator 快照；所有块分配/释放必须走该快照。
- 分配失败必须走 `nb_try_*` 或返回 `nb_status`；不要静默回退或假装成功。

## 平台代码

- 操作系统相关实现只放在 `src/platform/posix/` 或 `src/platform/win32/`。
- 共享代码（`src/*.c`）不得出现 `#ifdef _WIN32` 式平台分支；通过 `src/platform/nb_platform.h` 的钩子隔离。
- Windows 路径使用 UTF-8 ↔ UTF-16 的 Win32 W API；新增平台能力必须同时补 POSIX 和 Win32 实现。

## 验证

- 修改后至少运行：
  - `cmake --build build -j 8`
  - `ctest --test-dir build --output-on-failure`
  - ASan/UBSan：`cmake --build build-asan -j 8 && ctest --test-dir build-asan --output-on-failure`
- 涉及平台代码时，还要在 Docker + mingw-w64 下交叉编译，并用 Wine 跑测试。
- 新 API 必须有测试；修 bug 时优先加回归测试。
- 不要用弱化断言、关闭 sanitizer 或跳过测试来“修”失败。

## 公共 API 变更

- 先改头文件契约，再改实现和所有调用方。
- 删除或重命名 API 时，检查 `README.md`、`examples/`、`tests/` 中所有引用。
- 保持错误码、返回值和资源清理路径一致；失败路径不能泄漏或改变可观察状态。
