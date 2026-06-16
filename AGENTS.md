# AGENTS.md

## Bug 修复即时记录

本文件作用于当前仓库，用于同时给 Codex、Cursor 和其他支持仓库级指令的 AI 使用。

当任务属于修 bug、debug、构建失败修复、运行异常排查时，必须使用 `bugfix-record` 工作流。

要求：
- 排查前先查看 `memory/bugfix_records.md` 是否存在相关历史记录。
- 根因确认后，记录问题等级、现象、根因、解决方案、验证方式和相关文件。
- 未确认根因时不要写成结论，可以标记为 `investigating`。
- 修复完成后，如果 `memory/bugfix_records.md` 存在，追加记录；如果不存在，先请求确认是否创建。
- 最终回复中必须说明是否已记录，以及记录的等级、状态和标题。

工作流定义文件：
- `.agents/skills/bugfix-record/SKILL.md`
- `.cursor/rules/bugfix-record.mdc`
