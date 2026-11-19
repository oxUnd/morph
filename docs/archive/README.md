# 历史归档

这些材料保留了当时的需求、设计决策、验证环境和限制。使用当前版本时请从[现行文档](../README.md)进入。

| 历史材料 | 保留内容 | 当前入口 |
| --- | --- | --- |
| [需求文档](REQUIREMENTS.md)、[中文系统介绍](introduction.zh-CN.md) | 已完成的需求范围、原验收口径和早期系统图 | [用户手册](../morph.txt)、[开发约定](../../AGENTS.md) |
| [nbase 迁移](nothing-migration-assessment.zh-CN.md) | 迁移基线、兼容性修复和分发许可事项 | [构建](../../README.md#build)、[依赖与许可](../../AGENTS.md#vendor) |
| [稳定性改进](src-stability-improvement-plan.zh-CN.md)、[内存审计](memory-usage-audit.zh-CN.md) | 修复记录、测试边界和待验证的增长点 | [所有权](../state-ownership.md)、[资源边界](../model-capability-limits.md) |
| [会话同步复核](session-sync-review.md) | 跨端恢复问题、导入策略及当时的设备验证 | [备份与导入](../runtime-architecture.md#session-backups-and-import) |
| [Hooks 设计](hooks-design.md)、[旧权限说明](permissions.zh-CN.md) | 首次交付方案和旧配置示例 | [Hooks](../hooks.md)、[权限](../permissions-design.md) |
| [环境上下文](environment-context.md)、[模型历史设计](model-history-design.zh-CN.md)、[ReAct 时序图](react-loop-timing.html) | 早期提示词、压缩和执行模型 | [Prompt 组装](../prompt-architecture.md)、[会话历史](../cli-history.md)、[ReAct 实现](../../src/agent/README.md) |
| [内存管理](memory-management.md)、[记忆偏好](memory-preferences.md) | 早期内存生命周期和偏好模型 | [状态所有权](../state-ownership.md)、[记忆与检索](../memory.md) |
| [终端进程隔离复核](terminal-isolation-review.md) | 后台进程抢占终端的事件、已修复路径和其他启动器的复核项 | [后台命令](../async-exec.md)、[沙箱](../sandbox.md) |
