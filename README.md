<div align="center">

# LiteKVM

**零配置开源键鼠共享 —— 装上 → 附近电脑 → 输 PIN 配对 → 直接用**

[![CI](https://github.com/haoba55556-blip/deskflow/actions/workflows/ci.yml/badge.svg)](https://github.com/haoba55556-blip/deskflow/actions/workflows/ci.yml)
[![Version](https://img.shields.io/badge/version-0.1.0--alpha.1-orange)](CHANGELOG.md)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey)

</div>

> ⚠️ **本项目处于早期开发阶段**：下表所列特性均如实标注状态，当前还没有可用的稳定发布包。

## 这是什么？

LiteKVM 是一套零配置的开源键鼠共享（软件 KVM）工具：在一台电脑上装好 LiteKVM，它会自动发现同一局域网内的其他电脑；输入 PIN 码完成配对后，你就可以把鼠标"滑"到旁边的屏幕上，用同一套键盘鼠标无缝控制多台机器，还能跨机复制文本与文件。不需要 KVM 硬件，也不需要手动配置 IP 和端口。

一句话：**像连蓝牙耳机一样连上你旁边那台电脑。**

## English

LiteKVM is a free, open-source, zero-config software KVM: install it once, let mDNS automatically discover nearby computers, pair with a short PIN, then share one keyboard and mouse across machines — plus cross-machine clipboard and file transfer. No extra hardware, no manual network setup. LiteKVM is being built on top of our fork of [Deskflow](https://github.com/deskflow/deskflow); see [Acknowledgements](#-致谢与来源--acknowledgements).

## ✨ 特性一览 / Features

| 特性 | 说明 | 状态 |
| --- | --- | --- |
| 🔍 **mDNS 自动发现** | 同一局域网内自动发现附近设备，免手动填 IP | 🔨 开发中 |
| 🔐 **PIN 安全配对** | 类似蓝牙的短 PIN 码配对，密钥协商 + 加密传输 | 🔨 开发中 |
| 🧲 **多机磁吸布局同步** | 屏幕边缘磁吸式排布，多机之间自动同步布局关系 | 🔨 开发中 |
| 📋 **文件剪贴板** | 跨机复制粘贴文本与文件 | 🔨 开发中 |
| 🛰️ **自部署中继** | 自建中继服务器，实现跨网段互联 | 🗺️ 规划中 |

> 诚实声明：以上能力目前**全部处于开发中**，尚未提供正式发布版本。欢迎围观源码、提 Issue、一起把它做出来。

## 🚀 快速开始 / Quick Start

目前请从源码构建，三平台详细依赖与步骤见 **[docs/BUILDING.md](docs/BUILDING.md)**：

```bash
git clone https://github.com/haoba55556-blip/deskflow.git litekvm
cd litekvm
cmake -B build
cmake --build build
```

最低依赖：CMake ≥ 3.24、Qt ≥ 6.7、OpenSSL ≥ 3.0。

## 🗺️ 路线图

- [ ] 0.1.0-alpha.1 —— 品牌脚手架 + CI/Release 流水线（见 [CHANGELOG.md](CHANGELOG.md)）
- [ ] mDNS 发现 + PIN 配对最小可用链路
- [ ] 多机磁吸布局与剪贴板打通
- [ ] 首个公开 alpha 安装包
- [ ] 自部署中继服务

## 🤝 参与贡献

欢迎 PR！流程与提交规范见 [CONTRIBUTING.md](CONTRIBUTING.md)；报 bug / 提需求请使用 [Issue 模板](.github/ISSUE_TEMPLATE/bug_report.md)。

## 🙏 致谢与来源 / Acknowledgements

- 本项目 fork 自 [Deskflow](https://github.com/deskflow/deskflow)（Synergy 的开源社区延续项目），在其代码基础上进行 LiteKVM 品牌重塑与产品化开发。
- 衷心感谢 Deskflow 及其上游 Barrier / Synergy 社区多年的持续投入。
- 本仓库整体依据 **GPL-2.0** 发布，LiteKVM 继承并继续遵循同一许可证。

## 📄 许可证 / License

[GPL-2.0](LICENSE) —— 本仓库代码沿用上游 Deskflow 的 GNU General Public License v2。
