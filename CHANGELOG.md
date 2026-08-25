# Changelog

本项目的所有显著变更都将记录在本文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [0.1.0-alpha.1] - Unreleased

首个以 LiteKVM 品牌发布的里程碑（规划中，尚未发布）。

### 计划新增 / Added

- LiteKVM 品牌脚手架：README、中文构建指南 `docs/BUILDING.md`、贡献指南 `CONTRIBUTING.md`
- GitHub Actions CI 占位流水线：Windows (MSVC) / macOS (Clang) / Linux (GCC) 三平台矩阵构建
- Release 占位流水线：推送 `v*` 标签时构建产物并附加 SHA256 校验和
- 中文 Issue 模板：Bug 报告与功能建议

### 说明

- 本仓库 fork 自 [Deskflow](https://github.com/deskflow/deskflow)（GPL-2.0），继承其全部代码基线、历史与许可证。
- 当前所有特性（mDNS 自动发现 / PIN 安全配对 / 多机磁吸布局同步 / 文件剪贴板）均处于开发中；自部署中继为规划项。详见 README 特性表。

[0.1.0-alpha.1]: https://github.com/haoba55556-blip/deskflow/releases/tag/v0.1.0-alpha.1
