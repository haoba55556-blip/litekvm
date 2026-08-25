# LiteKVM 全开源实施方案 v2（对标 INGcontrol，纯开源版）

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** 基于 Deskflow 做一个**完全开源**的零配置键鼠共享项目：开箱即连、无账号无云依赖、可选自部署中继，功能体验对标 INGcontrol。

**Architecture:** 客户端 = Deskflow fork（C++20/Qt6）+ 零配置层（mDNS 发现 + PIN 配对 + 直连布局同步）；跨网中继 = 独立开源容器（docker-compose 一键起，地址+令牌直连，无中心服务器）。**整个项目没有任何闭源组件。**

**Tech Stack:** C++20 / Qt 6 / CMake（客户端）；Go + libpion/turn 或 coturn（自部署中继容器）；GitHub Actions（Win/Mac/Linux 三平台 CI）；mkdocs-material（文档站）。

---

## 一、v1→v2 变更记录

| 项目 | v1（商业版） | v2（全开源版） |
|---|---|---|
| 登录 | 手机号/微信 | ❌ 无登录，设备级 PIN 配对 |
| 账号/会员/支付 | Go 服务端+license | ❌ 全部删除 |
| 布局同步 | 云端 API | ✅ 设备间直连同步（P2P） |
| 文件剪贴板 | 会员收费 | ✅ 免费核心功能 |
| 跨网互联 | 官方中继 | ✅ 自部署开源中继容器 |
| 服务端代码 | 闭源 | ✅ 同仓库开源 |
| 收入模式 | 订阅/买断 | GitHub Sponsors 赞助（可选） |

**代价说明（已确认接受）：** 没有账号体系 = 没有"扫码即用"的跨网体验，跨网需用户自己跑一个中继容器或直连 IP。换来的是：无运营成本、无合规负担（不用办主体/备案/支付）、可进 Linux 发行版仓库。

## 二、决策记录（DR）

- **DR-1 底座**：Fork `deskflow/deskflow`（28.3k★，GPL-2.0，2026-08-23 活跃；Input Leap 已归档、Barrier 停更——2026-08-25 实测）。
- **DR-2 License**：整体 GPL-2.0（继承上游兼容）；文档 CC-BY-SA-4.0。全开源无闭源模块，GPL 合规零风险。
- **DR-3 身份模型**：无账号。每台设备生成持久密钥对（首次启动），配对=交换公钥+用户肉眼比对指纹/PIN（SSH known_hosts 模式）。
- **DR-4 MVP 场景**：局域网零配置互连。跨网中继放后期且为自部署形态。
- **DR-5 上游策略**：自有代码全部隔离在 `src/litekvm/`，每月 rebase 上游。
- **DR-6 差异化定位**：上游 deskflow 要手动填 server/client IP 配置文件，LiteKVM 主打「装上就能用」（mDNS 自动发现+一键配对）——这是独立开源贡献价值，也是与上游 rebase 时最需要守护的边界。

## 三、系统架构

```
┌─ 客户端 A（fork deskflow）─────┐      ┌─ 客户端 B ─────────────────┐
│ deskflow core（白拿：输入注入/  │◄────►│ 同左                        │
│ 协议/TLS/剪贴板文本）           │ mDNS │                             │
│ src/litekvm/                  │ 发现 │  首次配对：A显示6位PIN        │
│  discover/  mDNS 广播+扫描     │      │  B输入PIN → 交换公钥持久化    │
│  pairing/   PIN配对+信任库     │      │  之后：自动互信直连           │
│  mesh/      角色协商+布局同步  │◄────►│                             │
│  clipfile/  文件剪贴板分块传输 │      │                             │
└──────────────┬────────────────┘      └─────────────────────────────┘
               │ 可选：跨网时
     ┌─ 自部署中继容器（同仓库 /relay，Go）──┐
     │ docker compose up 一条命令            │
     │ 用户在两端粘贴 relay URL + token 即通  │
     └──────────────────────────────────────┘
```

## 四、实施计划

### Phase 0：环境搭建 + 编译跑通（~3 天）

#### Task 0.1: Windows 构建链
- 装 VS2022（C++ 桌面开发 workload）、CMake≥3.24、Ninja、Qt 6.5+ msvc 版、Python3
- 验证：`cmake --version && ninja --version`

#### Task 0.2: Fork 并克隆
- GitHub fork `deskflow/deskflow` → 克隆到 `D:\projects\litekvm`（`--recurse-submodules`）
- 分支 `litekvm/main` 为产品主线；`git remote add upstream https://github.com/deskflow/deskflow.git`
- 验证：`git remote -v` 有 origin+upstream

#### Task 0.3: 编译通过
- 按仓库 `BUILD.md` 实际指引：`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt路径> && cmake --build build`
- 验证：`build\bin\deskflow.exe` 启动正常

#### Task 0.4: 双机冒烟
- 本机 server + 第二台机器 client（手动配置即可，此时还是原生功能）
- 验证：鼠标滑过屏幕边界控制第二台机
- ✅ 里程碑：核心价值成立

### Phase 1：品牌换皮（~1 周）

#### Task 1.1: 改名 LiteKVM
- 全局替换品牌字符串+图标；协议握手 magic string 和默认端口改掉（24800→25900）防误连官方版
- 验证：GUI 显示 LiteKVM；官方 deskflow 连不上新版

#### Task 1.2: 开源项目门面
- README.md（双语：中文为主+English）、CONTRIBUTING.md、LICENSE（GPL-2.0）、CODE_OF_CONDUCT.md、CHANGELOG.md
- GitHub 仓库配置：issue 模板、discussion 开区、topic 打标签
- 验证：陌生人看 README 30 秒能明白这是啥怎么装

#### Task 1.3: 三平台 CI
- GitHub Actions matrix：Windows(msvc) + macOS(clang) + Linux(gcc/AppImage)；Release 自动附产物+SHA256
- 注：macOS 无签名也能分发（用户右键打开或 xattr 清除），README 写清楚；有预算再上 Apple Developer $99/年
- 验证：打 tag 自动出三平台安装包

### Phase 2：零配置发现与配对（~2 周，本项目灵魂）

#### Task 2.1: mDNS 发现
- `src/litekvm/discover/`：用 Qt Network 的 QDnsServiceBrowse（或 mdns.h 单头库）广播 `_litekvm._tcp.local.`，携带设备名/平台/指纹哈希
- UI：主窗口「附近的电脑」列表实时刷新
- 验证：两台机器互相出现在对方列表，<3 秒

#### Task 2.2: PIN 配对与信任库
- `src/litekvm/pairing/`：点击对方设备 → 对方弹窗显示 6 位数字+本机指纹 → 本机输入 → 双方交换 Ed25519 公钥存入 `%APPDATA%/LiteKVM/trust.json`（macOS ~/Library/Application Support）
- 已配对设备列表管理（移除/改名）
- 验证：配对一次后重启双方仍互信；删掉信任记录后要求重新配对

#### Task 2.3: 自动角色协商
- `src/litekvm/mesh/`：已配对设备间按 device_id 字典序自动定 server/client 角色，无需用户理解概念
- 保留「高级设置」露出手动配置（兼容 deskflow 原生用法，也是上游合并的安全垫）
- 验证：两台新机配对完成后 10 秒内自动连上，全程只输入了一次 PIN

#### Task 2.4: 多机组网
- 与第 3 台配对后自动入网；布局按配对顺序默认排列
- 验证：三台机器串联，鼠标连续滑过三块屏

### Phase 3：布局直连同步（~1 周）

#### Task 3.1: 布局编辑器
- 「电脑排列」卡片拖拽界面（Qt Graphics View），磁吸对齐
- 布局变更通过已建立的加密连接广播给所有已配对设备（每台都存全网拓扑副本，CRDT 式 last-write-wins）
- 验证：A 机拖动布局，B/C 机 5 秒内生效；断网的机器重连后补齐最新布局

### Phase 4：文件剪贴板（~2 周）

#### Task 4.1: 文件传输通道
- `src/litekvm/clipfile/`：监听剪贴板文件事件（Win CF_HDROP / mac NSPasteboard file URLs）→ 元数据+256KB 分块（sha1 校验）复用 deskflow 加密数据通道 → 目标机落盘临时目录并挂到剪贴板引用
- >100MB 弹进度条可取消
- 验证：A 复制 50MB 文件夹 → B Ctrl+V 完整还原，sha1 一致

#### Task 4.2: 跨系统路径与编码坑
- 中文文件名 UTF-8 统一处理；Win↔Mac 换行符/权限位忽略策略
- 验证：中文名文件夹双向传输不乱码

### Phase 5：自部署跨网中继（~2 周）

#### Task 5.1: 中继容器
- `/relay` Go 实现（libpion/turn）：token 鉴权（配对时生成的共享密钥）、纯转发不解析内容
- docker-compose.yml 一条命令起；README 给 VPS 部署教程（4G 内存小鸡就够）
- 客户端设置页加「自定义中继」：URL+token，两端一致即通
- 验证：手机热点 vs 家宽两网互连延迟 <80ms（同城）

#### Task 5.2: 发布工程收尾
- winget manifest + Homebrew tap 提交；flathub 评估
- mkdocs-material 文档站（GitHub Pages）：安装/配对/自建中继/FAQ/编译指南
- 验证：`winget install litekvm`（审核通过后）/ 文档站可访问

## 五、里程碑（solo 全职）

```
Week 1     Phase 0+1   能装能用有自己的名字+CI
Week 2-3   Phase 2     零配置发现+PIN配对 ← 核心差异化成型
Week 4     Phase 3     布局直连同步
Week 5-6   Phase 4     文件剪贴板
Week 7-8   Phase 5     自部署中继+发布工程 → v1.0 公测
```

比商业版快约 2 周（没有账号/支付/license）。

## 六、风险与对策

| 风险 | 对策 |
|---|---|
| 上游 rebase 冲突 | DR-5 隔离 + 每月固定 rebase 日；discover/mesh 尽量少碰 core 文件 |
| mDNS 在某些路由器被 AP 隔离禁掉 | 检测+提示用户；提供手动 IP 直连兜底（deskflow 原生能力） |
| macOS 无签名劝退小白 | README 图文教程；攒 star 后再考虑签名费用 |
| GPL-2.0 合规 | 全部开源无灰色地带；第三方依赖引入前查 license 兼容性 |
| 三平台维护精力 | CI 先保证 Win+Mac，Linux 用上游已有的 AppImage 流水线改造 |

## 七、明确不做（YAGNI）

- 账号体系、会员、支付、官方云服务（用户明确要求全开源）
- 微信登录（用户明确移除）
- 音视频远控
- 移动端 App
- 多语言（v1 中文文档为主+英文 README）

## 八、发布渠道清单

- GitHub Releases（主）+ SHA256 校验（学 INGcontrol 的校验习惯）
- winget / Homebrew tap / Flathub（Phase 5.2）
- 酷安/少数派/V2EX 发帖（中文社区首发），r/desktops 类英文社区跟进
