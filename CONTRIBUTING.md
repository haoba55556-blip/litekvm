# 贡献指南 / Contributing to LiteKVM

感谢关注 LiteKVM！无论报 Bug、提需求还是交代码，都欢迎。本文是精简版流程，动手前请花两分钟读完。

## 贡献流程（Fork → Branch → Commit → PR）

1. **Fork** 本仓库到你自己的账号下，然后 clone 你的 fork。
2. **创建分支**：基于 `litekvm/main` 拉出功能分支，命名建议：
   - `feat/<主题>`：新功能
   - `fix/<主题>`：修 Bug
   - `docs/<主题>`：文档
   - `chore/<主题>`：构建、CI 等杂项
3. **提交代码**：commit message 遵循 [Conventional Commits](https://www.conventionalcommits.org/zh-hans/v1.0.0/)。
4. **发起 PR**：目标分支为 `litekvm/main`，描述清楚改了什么、为什么改、如何验证。
5. CI 通过并经至少一名维护者 review 后合并。

## Conventional Commits 速查

| 类型 | 用途 | 示例 |
| --- | --- | --- |
| `feat` | 新功能 | `feat: add mdns discovery for nearby hosts` |
| `fix` | 修复缺陷 | `fix: pin pairing fails on ipv6-only network` |
| `docs` | 文档变更 | `docs: rewrite building guide for linux deps` |
| `refactor` | 重构（不改行为） | `refactor: extract pairing handshake module` |
| `test` | 测试相关 | `test: add unit tests for layout sync` |
| `chore` | 构建/CI/杂项 | `chore: rebrand scaffold as LiteKVM` |
| `ci` | CI 流水线 | `ci: enable qt cache on windows runner` |

可选加作用域，如 `feat(discovery): ...`；破坏性变更请在正文注明 `BREAKING CHANGE:`。

## 分支与远程说明

- `litekvm/main`：本项目主开发分支，所有 PR 合入这里。
- `origin`：你的 fork；`upstream`（deskflow/deskflow）仅用于同步上游更新，**不要**向上游推代码。

## 提交前检查清单

- [ ] 本地构建通过：`cmake -B build && cmake --build build`
- [ ] 新增第三方依赖已在 PR 中说明理由与许可证
- [ ] commit message 符合上面的类型规范
- [ ] 改动只涉及本仓库范围，未夹带无关文件

## 反馈问题

- 报 Bug 用 [Bug 模板](.github/ISSUE_TEMPLATE/bug_report.md)，提需求用 [功能建议模板](.github/ISSUE_TEMPLATE/feature_request.md)。
- 提问前请先搜索已有 Issue，避免重复。
