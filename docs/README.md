# Morph 文档

Morph 在终端中运行多模态 Agent。交互式 CLI、单次命令和可选的 FastCGI 服务共用同一套 runtime，负责模型调用、工具执行、权限和会话持久化。初次使用可从[构建与启动](../README.md#build)、[配置向导](setup.md)和[随程序安装的用户手册](morph.txt)开始。

## 使用与配置

| 要做的事 | 文档 |
| --- | --- |
| 配置角色、模型或 MCP 服务 | [首次配置与重新配置](setup.md)、[模型默认值](setup-model-defaults.md)、[MCP 预设](setup-mcp.md)、[配置示例](../config.toml.example) |
| 安装技能，管理 MCP 连接 | [命令行管理](cli-management.md)、[MCP 协议兼容性](mcp-compatibility.md) |
| 继续会话、查历史、导出回答、撤回修改 | [会话历史](cli-history.md)、[分享会话](cli-share.md)、[检查点与回退](checkpointing.md) |
| 查看工具执行，回答问题，收取任务结果 | [工具动态](cli-tool-feed.md)、[用户问答](ask-user.md)、[后台命令](async-exec.md)、[定时任务与收件箱](scheduled-tasks.md) |
| 查看记忆或调整偏好，使用协作代理 | [记忆与检索](memory.md)、[多代理协作](multi-agent.md) |
| 控制授权和自动化 | [权限模式](permissions-design.md)、[沙箱](sandbox.md)、[生命周期 Hook](hooks.md)、[Guardrail](guardrails.md)、[macOS 应用启动](macos-launch-adapter.md) |
| 读取目录和文件，创建工具 | [目录分页](file-list.md)、[文件读取](file-read.md)、[QuickJS 工具](quickjs-tools.md) |
| 在 Windows 构建或部署 Web 入口 | [Cygwin 构建](windows-cygwin.md)、[FastCGI 部署与接口](../src/sapi/fastcgi/README.md) |

## 开发与维护

| 主题 | 文档 |
| --- | --- |
| 执行边界和恢复 | [Runtime 架构](runtime-architecture.md)、[ReAct 实现](../src/agent/README.md)、[统一事件](event-system.md)、[上下文与进程恢复](context-and-process-recovery.md) |
| 模型输入和预算 | [Prompt 组装](prompt-architecture.md)、[模板语法](prompt-templates.md)、[角色提示词](role-prompts.md)、[模型能力与资源限制](model-capability-limits.md) |
| CLI 和底层类型 | [应用布局](tui-application-layout.md)、[CLI 模块地图](../src/sapi/cli/README.md)、[状态所有权](state-ownership.md)、[Unicode](unicode.md) |
| 开发规范和验证 | [开发指南](../AGENTS.md)、[权限模块](../src/permission/README.md)、[权限验收与复现](permissions-acceptance.md)、[开发待办](../TODO.md) |

已完成的需求、迁移计划、审计测量和早期设计保存在[历史归档](archive/README.md)。
