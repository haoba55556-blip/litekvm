# LiteKVM 构建指南 / Building LiteKVM（中文）

本文介绍如何从源码构建 LiteKVM。当前代码基线来自上游 Deskflow，构建系统沿用 CMake；品牌与二进制重命名将随版本迭代逐步完成。

> 💡 英文原始文档见 `docs/dev/build.md`（上游），本文是面向 LiteKVM 用户的中文改写版，并补充了常见问题。

## 环境要求总览

| 依赖 | 最低版本 | 平台 | 说明 |
| --- | --- | --- | --- |
| [CMake](https://cmake.org/) | **3.24+** | 全平台 | 构建系统 |
| [Qt](https://www.qt.io/) | **6.7.0+** | 全平台 | GUI 框架 |
| [OpenSSL](https://www.openssl.org/) | **3.0+** | 全平台 | 加密传输 |
| [libportal](https://github.com/flatpak/libportal) | 0.9.1+ | Linux/BSD | 桌面集成 |
| [libei](https://gitlab.freedesktop.org/libinput/libei) | 1.3+ | Linux/BSD | 输入注入 |

编译器要求：

- **Windows**：Visual Studio 2022（MSVC），必须勾选「使用 C++ 的桌面开发」工作负载
- **macOS**：Xcode Command Line Tools（AppleClang）
- **Linux**：GCC 或 Clang 近年版本即可

> 部分缺失的依赖 CMake 会在配置阶段自动拉取，但提前装好可以节省大量时间。

## Windows

### 1. 安装 Visual Studio 2022

- 下载安装 VS2022 Community 及以上版本；
- 安装时**必须**勾选 **「使用 C++ 的桌面开发」(Desktop development with C++)** 工作负载（包含 MSVC 编译器与 Windows SDK）；
- 单独安装 [CMake ≥ 3.24](https://cmake.org/download/)（VS 自带的 CMake 也可用，但建议独立安装最新版）。

### 2. 安装 Qt ≥ 6.7（推荐用 aqt 命令行安装）

推荐使用 [aqtinstall](https://github.com/miurahr/aqtinstall)，比官方在线安装器更快、更省事：

```powershell
pip install aqtinstall
aqt install-qt windows desktop 6.7 win64_msvc2019_64 -O C:\Qt
```

安装完成后（示例路径按实际小版本调整）：

- 把 `C:\Qt\6.7.x\msvc2019_64\bin` 加入 `PATH`；
- 构建时通过 `-DCMAKE_PREFIX_PATH=C:\Qt\6.7.x\msvc2019_64` 告诉 CMake Qt 的位置（或把 `C:\Qt\6.7.x\msvc2019_64\lib\cmake` 加入 `PATH`）。

其他可选方式：

- **Qt 官方在线安装器**：勾选 Qt 6.7+ 的 MSVC 组件；
- **vcpkg 托管 Qt**：配置时加 `-DVCPKG_QT=ON`。⚠️ 首次会从源码编译 Qt，可能耗时数小时；切回系统 Qt 时需删除生成的 `vcpkg.json` 与 `build` 目录后重新配置。

⚠️ **不要同时保留两种 Qt 安装方式**——混用会出现库和插件各取一处的诡异问题。切换前先卸载旧安装。

### 3. OpenSSL

- 可用 vcpkg：`vcpkg install openssl`；
- 或使用 slproweb 等预编译发行版；
- CMake 找不到时用 `-DOPENSSL_ROOT_DIR=<openssl根目录>` 显式指定。

### 4. 构建

在 **「x64 Native Tools Command Prompt for VS 2022」** 中执行：

```bat
cmake -B build
cmake --build build --config Release
```

## macOS

```bash
# 编译器与工具链
xcode-select --install

# 依赖
brew install cmake ninja qt openssl@3

# 配置 + 构建
cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
```

开发签名（可选）：本地调试可用 `-DAPPLE_CODESIGN_DEV="Apple Development: you@example.com (XXXX)"` 指定开发证书（用 `security find-identity -v -p codesigning login.keychain-db` 查询）。该选项仅用于本地开发，不适用于分发签名。

## Linux（Ubuntu 22.04+ / Debian）

```bash
sudo apt update
sudo apt install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-tools-dev-tools libqt6svg6-dev libssl-dev \
  libx11-dev libxtst-dev libxkbfile-dev libxi-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libgl1-mesa-dev \
  libportal-dev libei-dev

cmake -B build
cmake --build build
```

Fedora 对应包名：`gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel openssl-devel libX11-devel libXtst-devel libxkbfile-devel libportal-devel libei-devel`（以实际仓库为准）。

## 常见问题 FAQ

**Q：CMake 报错 "CMake 3.24 or higher is required"？**
升级 CMake：官网下载新版，或 `pip install cmake`、`brew upgrade cmake`；Windows 下确认命令行里 `cmake --version` 指向新装的版本而非 VS 自带旧版。

**Q：报错找不到 Qt6（Could not find a package configuration file provided by "Qt6"）？**
用 `-DCMAKE_PREFIX_PATH=<Qt安装目录>` 指向 Qt 根目录，或设置 `Qt6_DIR` 到 `<Qt>/lib/cmake/Qt6`。Windows 下最常见原因是 PATH 里没有 Qt，或同时存在多套 Qt 安装互相干扰。

**Q：Windows 下 CMake 选不到 MSVC 编译器？**
在 VS2022 的「Developer Command Prompt」里运行 cmake，或显式指定生成器：`cmake -B build -G "Visual Studio 17 2022"`。

**Q：找不到 OpenSSL？**
加 `-DOPENSSL_ROOT_DIR=/path/to/openssl`（macOS Homebrew 用户通常是 `$(brew --prefix openssl@3)`）。

**Q：Linux 上配置阶段提示 libportal / libei 版本不够？**
Ubuntu 22.04 自带版本偏旧，建议使用 24.04+ 或自行编译安装新版；这两项仅在 Linux/BSD 需要。

**Q：构建出来的程序还叫 deskflow？**
是的。品牌重塑分阶段进行，当前二进制名与内部标识仍沿用上游 Deskflow，后续迭代会统一改为 litekvm。

## 打包（可选）

```bash
cmake --build build --target package
```

按平台可生成 archive/deb/rpm/dmg/msi 等安装包格式（详见上游 `docs/dev/build.md`）。
