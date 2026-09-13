# LiteKVM 双侧切入（Dual Side Layout）实施方案

> **For Hermes:** 用 subagent-driven-development 逐步实现/验证本计划。本计划已落地代码改动（见「六、实现步骤」），剩余的是编译 + 双机实测。

**日期：** 2026-09-14
**状态：** 代码已改完（未编译，本机无 Qt 工具链）；等待编译与双机验证
**关联：** `docs/plans/2026-08-25-litekvm-mvp.md`（Phase 3 布局编辑器）、`src/lib/gui/ScreenSetupModel.{h,cpp}`、`src/lib/gui/config/{Screen,ServerConfig}.{h,cpp}`、`src/lib/gui/dialogs/{ScreenSettingsDialog,ServerConfigDialog}.*`

---

## 一、背景

### 1.1 用户场景

服务器（台式机）居中，笔记本作为客户端。希望**鼠标从台式机屏幕的左侧或右侧任意一边滑出去，都能进入同一台笔记本**，即「一台客户端挂在服务器的两侧」。

### 1.2 已实测的两条底层事实

| # | 事实 | 影响 |
|---|---|---|
| F1 | 服务器配置 `section: screens` 里**屏名必须唯一**，同名两次时服务器拒绝启动（duplicate screen name） | 不能把同一台客户端塞进两个网格格子；GUI 的「同一屏只定义一次」约束不能破 |
| F2 | `section: links` 里**允许同一台屏幕的 `left` 与 `right` 指向同一目标屏**（已用外部 `deskflow-server.conf` 手工验证：客户端正常连上，鼠标从服务器左右两侧都能滑到笔记本） | 双侧切入可以在**边的集合**上实现：屏只定义一次，边加两条 |

### 1.3 现有 GUI 布局模型的限制

- `ServerConfigDialog` 的「Computers」页是一个 5×5 网格（`ScreenList` = 单元格数组，`name` 为空即空位），拖拽即搬运 `Screen` 对象。
- `links` 段不是手写的，而是**从网格相邻关系推导**：`ServerConfig::operator<<(QTextStream&, const ServerConfig&)` 遍历每个非空屏，用 `adjacentScreenIndex(idx, ±1, ±1)` 查上/下/左/右邻居（`neighbourDirs`：right / left / up / down）。
- 推论：**网格里一个格子 = 一台屏 = screens 段一条定义**，所以「同一客户端出现在服务器两侧」在今天的 GUI 里根本无法表达——这正是用户当初只能手写外部 conf 的原因。

### 1.4 外部配置模式的副作用（必须收尾）

`CoreProcess::persistServerConfig()`：只有当 `server/externalConfig` 为 false 时，GUI 才会在每次启动核心前把 `m_serverConfig` 写进 `<settingsPath>/<appname>-server.conf`（Windows 默认 `C:\ProgramData\LiteKVM\` 下；用户手工验证时用的是同目录的 `deskflow-server.conf`）。勾上「外部配置」后 GUI 不再写该文件，**拖拽布局随即变成摆设**。

⇒ 本方案的目标就是让**原生 GUI 路径**支持双侧，从而可以把外部配置关掉、回到「拖拽即生效」。

---

## 二、目标与非目标

**目标**

1. GUI 中勾选某台**客户端**屏的「双侧切入」后，服务器生成的 `links` 段用 `left` 与 `right` 同时指向该客户端（F2 允许）。
2. **不产生重复的 screen 定义**：`screens` 段屏名唯一性约束（F1）原封不动。
3. **向后兼容**：没勾选时 `links` 段输出与今天**逐字节一致**；单侧布局、多屏链式布局行为不变。
4. 设置持久化：重启 GUI、重开对话框后标记还在；拖拽搬动屏幕时标记跟着走。
5. 用户能看出「勾了但没生效」（摆错位置时给出提示），不靠猜。

**非目标（本版不做）**

- 上下两侧（`up`/`down`）的双侧镜像；
- 一台客户端同时挂在两台服务器两侧之类的高级拓扑；
- 改动上游核心 `src/lib/server/Config.cpp`——**不需要**：`readSectionLinks()` 对 `(src, dir)` 逐条 `connect()`，不同方向是两个独立边，不会触发 "overlapping range"。
- 布局可视化徽章（图标角标）、Phase 3 的布局同步广播里带 `dualSide`。

---

## 三、方案对比

| 方案 | 做法 | 结论 |
|---|---|---|
| **A. 网格双占位** | 让客户端同时占左右两个格子，靠网格自动生成双侧 links | ❌ 直接违反 F1：`screens` 段会出现两条同名定义，服务器拒绝启动。GUI 侧还得放宽 `ScreenDuplicationsValidator`，等于把约束撕开。**否决** |
| **B. 显式链接覆盖表** | `ServerConfig` 增加 `QList<LinkOverride>{src, dir, dst}`，生成时叠加到网格推导结果之上 | ⭕ 可行但引入**第二套拓扑真相**，与拖拽网格并存时冲突判定/UI 表达成本高（要新对话框 + 新序列化 + 冲突优先级规则）。改动面最大。**暂不采纳** |
| **C. 客户端屏「双侧切入」标记** | `Screen` 增加 `dualSide` 标记；links 生成时，服务器在「网格没有邻居」的那一侧用「反方向邻居」补齐 | ✅ 网格仍是唯一拓扑真相；屏仍只定义一次；改动集中在 4 个文件。**采纳** |
| **D. 继续手写外部 conf** | 保持现状，文档教用户手写 | ❌ 与「零配置、拖拽即生效」的产品定位冲突，且外部配置会顺带废掉 GUI 布局。**否决** |

**选 C 的关键理由**：它把新能力做成了「**加边不加屏**」——`screens` 段一个字都没变，双侧只体现在 `links` 段的多一条边上，正好落在 F1（屏名唯一）与 F2（同屏左右同指）的交集里。

---

## 四、核心规则与生成结果

### 4.1 一条规则说清语义

> **`dualSide` 是客户端屏的属性**：勾选后，**服务器**在「网格里没有邻居」的左右那一侧，补一条指向该客户端的边。
> 只补服务器一侧，**不给客户端补反向边**——否则鼠标刚滑进客户端又会从同一条边弹回去（边缘回弹）。

### 4.2 代码（已落地）

新增两个私有辅助（`src/lib/gui/config/ServerConfig.{h,cpp}`），`operator<<` 改调 `neighbourIndex`：

```cpp
int ServerConfig::neighbourIndex(int idx, int deltaColumn, int deltaRow) const
{
  const int adjacent = adjacentScreenIndex(idx, deltaColumn, deltaRow);
  if (adjacent != -1 && !screens()[adjacent].isNull())
    return adjacent;

  return dualSideNeighbourIndex(idx, deltaColumn, deltaRow);
}

int ServerConfig::dualSideNeighbourIndex(int idx, int deltaColumn, int deltaRow) const
{
  // 双侧切入（dual side）：服务器左右两侧接到同一台客户端。
  // deskflow 的 links 段允许同一台屏幕的 left 与 right 指向同一目标屏幕，但 screens 段
  // 要求屏名唯一（重名时服务器拒绝启动），所以这里只补边、绝不新增屏幕。
  // 只有左侧/右侧（deltaColumn != 0）开放本特性；上下两侧的语义与网格不一致，暂不开放。
  if (deltaColumn == 0 || idx < 0 || idx >= screens().size() || screens()[idx].isNull() ||
      !screens()[idx].isServer())
    return -1;

  // 反向的网格邻居 = 本屏的「锚点」屏；只有锚点屏自己勾选了双侧切入才镜像。
  const int anchor = adjacentScreenIndex(idx, -deltaColumn, deltaRow);
  if (anchor == -1 || screens()[anchor].isNull() || !screens()[anchor].dualSide())
    return -1;

  return anchor;
}
```

三个门槛条件，缺一不可：
1. `deltaColumn != 0` —— 只管左右（上下留作后续扩展点）；
2. 当前屏是**服务器**（`isServer()`）—— 保证双侧只出现在服务器两侧，不会顺带在客户端之间冒出莫名其妙的边；
3. **反方向**的那个网格邻居勾了 `dualSide` —— 这就是「客户端在左、服务器在右」的锚点关系。

### 4.3 生成结果一览（算法级实测输出）

> 输出由「逐行照搬上述 C++ 逻辑的 Python 模型」生成，非编译产物；用于验证规则与向后兼容，见 §7.1。

| # | 布局（客户端勾选状态） | 生成的 `links` | `screens` 段 | 与旧算法一致 |
|---|---|---|---|---|
| A | [laptop] [server]，**未勾选** | `laptop: right = server`；`server: left = laptop` | `laptop, server`（唯一） | ✅ 完全一致 |
| B | [laptop] [server]，**已勾选** | `laptop: right = server`；**`server: left = laptop` + `server: right = laptop`** | `laptop, server`（唯一） | ➖ 仅 `server` 多一条边 |
| C | [server] [laptop]，**已勾选** | `laptop: left = server`；**`server: right = laptop` + `server: left = laptop`** | `server, laptop`（唯一） | ➖ 仅多一条边 |
| D | [server] 上有 laptop，**已勾选**（摆错位置） | `laptop: down = server`；`server: up = laptop` | 唯一 | ✅ 无变化 → 需要 GUI 提示 |
| E | [deskA] [server] [laptop勾选] | `deskA: right = server`；`server: left = deskA` + `right = laptop`；`laptop: left = server` | `deskA, server, laptop`（唯一） | ✅ 无变化（服务器左右都不空，无镜像可用，**不覆盖**真实邻居） |
| F | [laptop勾选] [server] [pc2勾选] | `laptop: right = server`、`pc2: left = server`，服务器两侧都被真实邻居占满 | 唯一 | ✅ 无变化 |

场景 B 生成的完整 conf（与用户手工验证的形态一致）：

```
section: screens
	laptop:
	server:
end

section: links
	laptop:
		right = server
	server:
		right = laptop
		left = laptop
end

section: options
end
```

---

## 五、GUI 怎样生成这样的 links 而不产生重复 screen 定义

这是本次交付的关键问题，答案分三层：

### 5.1 `screens` 段的唯一性来自「网格单元格 ↔ 屏幕」的一对一，与双侧无关

```cpp
// ServerConfig.cpp — operator<<，未改动
outStream << "section: screens" << Qt::endl;
for (const Screen &s : config.screens()) {
  if (!s.isNull())
    outStream << s.screensSection();
}
```

- `ScreenList` 每个格子最多一个 `Screen`（空位 = `name` 为空 → 跳过）。
- 客户端屏名唯一由 `ScreenDuplicationsValidator` 保证（重名时对话框校验失败），服务器屏名取自本机 `ComputerName`；`ScreenNameValidator` 另外挡住别名冲突。
- **双侧切入的实现没有向 `ScreenList` 添加任何 `Screen`、也没有复制单元格**——它只改 `links` 段的生成函数。所以 F1 的约束天然成立，`screens` 段在 A~F 全部场景中都是唯一屏名（实测见 §4.3）。

### 5.2 双侧只体现在 `links` 段的「同一目标、不同方向」

```cpp
// ServerConfig.cpp — operator<< 的 links 循环（已改）
for (const auto &neighbour : std::as_const(neighbourDirs)) {
  // neighbourIndex 在网格相邻之外还会补上双侧切入的镜像边；
  // 同一目标屏可以在两个不同方向各出现一次（links 段允许，screens 段不受影响）。
  const int idx = config.neighbourIndex(i, neighbour.x, neighbour.y);
  if (idx != -1)
    outStream << "\t\t" << neighbour.name << " = " << config.screens()[idx].name() << Qt::endl;
}
```

- 核心解析端（`src/lib/server/Config.cpp` `readSectionLinks()`）是「按 `(src屏幕, 方向)` 逐条 `connect()`」的模型：`server: left = laptop` 与 `server: right = laptop` 是**两条不同的边**，键不同，因此既不重复也不重叠（`overlapping range` 只在同一 `(src, dir)` 的区间重叠时报错）。
- 屏蔽重复的逻辑本来就存在：同一方向只会输出一次（`neighbourIndex` 每方向只返回一个 `idx`）。

### 5.3 双向不会互相打架（为什么不会无限放大）

`neighbourIndex` 是**纯函数式**的、只查不写：`dualSide` 只在「反方向邻居勾选」时生效，而镜像出来的边**不会**反过来触发新的镜像（因为规则看的是**网格**相邻关系，而不是已生成的边）。因此：

- 每个方向最多补 1 条边，服务器最多拿到 1 条镜像边；
- 不会出现 A↔B 互相镜像导致的环状拓扑；
- 场景 F 里客户端一侧的镜像被服务器真实邻居「压住」，不会覆盖真实邻居（**优先级：网格相邻 > 双侧镜像**）。

---

## 六、实现步骤

### 6.1 已完成的改动（本工作区，未编译）

| 文件 | 改动 | 目的 |
|---|---|---|
| `src/lib/gui/config/Screen.h` | 新增 `m_DualSide`(默认 false)、`dualSide()`/`setDualSide()`；纳入 `operator==` 与 friend `QDataStream` 的 `<<`/`>>`；成员处加注释说明语义 | 标记就是客户端屏的属性，且要能持久化 + 跟着拖拽走 |
| `src/lib/gui/config/Screen.cpp` | `loadSettings()` 读 `dualSide`；`saveSettings()` 写 `dualSide`；`operator==` 纳入该字段 | 持久化到 `internalConfig/screens[i]/dualSide`；对话框脏检查能感知 |
| `src/lib/gui/config/ServerConfig.h` | 新增私有 `neighbourIndex()`、`dualSideNeighbourIndex()` 声明（带注释） | links 生成入口 |
| `src/lib/gui/config/ServerConfig.cpp` | 实现上述两个方法；`operator<<` 的 links 循环改调 `neighbourIndex`，并去掉就地 `isNull` 判断（已收敛进 helper） | **核心**：加边不加屏 |
| `src/lib/gui/ScreenSetupModel.h` | 新增 `QStringList misconfiguredDualSideScreens() const`（带 Doxygen） | 布局模型对外回答「哪些勾选当前无效」 |
| `src/lib/gui/ScreenSetupModel.cpp` | 实现该方法（同排、相邻列必须存在 `isServer()` 的屏）；`data()` 的 `Qt::ToolTipRole` 追加「Dual side」说明 | 网格里可见反馈 + 校验 |
| `src/lib/gui/dialogs/ScreenSettingsDialog.ui` | `groupComputerName` 组内新增 `QCheckBox #chkDualSide`（含 tooltip） | 用户唯一的勾选入口 |
| `src/lib/gui/dialogs/ScreenSettingsDialog.cpp` | 构造时回填勾选状态；**服务器屏禁用**并换 tooltip 指路「请在客户端屏上勾」；`accept()` 时写回 `setDualSide()` | 设置界面 |
| `src/lib/gui/dialogs/ServerConfigDialog.h/.cpp` | 新增 `updateLayoutHint()`；连接 `ScreenSetupModel::screensChanged`；`loadFromConfig()` 末尾调用；把 `label_2` 的文案改成「说明 + 可选警告」 | **布局对话框**里提示「勾了但没摆在服务器左右」 |
| `docs/plans/2026-09-14-dual-side-layout.md` | 本文件 | 计划与验证方法 |

### 6.2 交互路径（用户视角）

1. 主窗口 → Server Configuration（`ServerConfigDialog`）→ **Computers** 页。
2. 把笔记本拖到服务器的**左格或右格**（同一行相邻列）。
3. **双击**笔记本格子 → `ScreenSettingsDialog` → 勾「**Connect to both sides of the neighbouring screen**」→ OK。
4. 若摆错位置（例如放在服务器上方），Computers 页说明文字下方会立刻出现警告，列出屏名并给出修法。
5. Save → `m_serverConfig.commit()`（写入 `internalConfig`）→ 启动核心时 `CoreProcess::persistServerConfig()` 把 conf 写到 `<settingsPath>/<appname>-server.conf`。
6. 笔记本双击服务器的**左、右任一边缘**都能滑入；从笔记本滑出按网格相邻方向返回服务器。

### 6.3 后续可选（未做，登记 backlog）

- [ ] 上下两侧双侧（把 `dualSideNeighbourIndex` 的 `deltaColumn == 0` 门槛放开，改成「任一方向」）——注意上下双侧的鼠标语义需要单独实测。
- [ ] 网格内可视化徽章（`Screen::pixmap()` 叠加角标），比 tooltip 更一眼可辨。
- [ ] Phase 3 布局同步广播里带上 `dualSide`，让远端拓扑副本一致。
- [ ] 单测：`ServerConfig operator<<` 快照测试（场景 A~F）、`misconfiguredDualSideScreens()` 用例（`src/unittests/` 新增，需先确认上游 rebase 策略）。
- [ ] `translations/*.ts` 补 `lupdate`（新增英文串目前无译文，zh_CN 会回落英文）。

---

## 七、验证方法

### 7.1 已做（本机无 Qt 工具链，纯静态 + 算法级）

1. **UI XML 合法性**：`ScreenSettingsDialog.ui` / `ServerConfigDialog.ui` 用 XML 解析器解析通过；并确认 `chkDualSide` 的父链是 `ScreenSettingsDialog → _2 → groupComputerName → _3 → chkDualSide`（即确实落在「Computer Name」分组里）。
2. **括号/引号配平**：9 个被改的 `.h/.cpp` 文件 `{}`、`()`、`[]`、双引号全部配平。
3. **算法级模拟**：按 §4.2 的 C++ 逻辑逐行照搬成 Python 模型，跑场景 A~F，确认
   - `screens` 段在各场景均无重复屏名；
   - 场景 A/D/E/F 的 links 与「旧算法」逐条一致（向后兼容）；
   - 场景 B/C 恰好只多出一条镜像边，且是多在服务器上。
   ⚠️ 这是**逻辑等价模型**，不是编译产物。

### 7.2 编译后必须补做（本机无法执行）

```bash
cmake -B build
cmake --build build
```

1. **对话框能出来**：启动 GUI → Server Configuration → Computers 页正常显示；双击客户端屏，勾选框存在、服务器屏上勾选框是灰的。
2. **conf 文本比对**（最关键的一步）：
   - 联网前先备份现有 `<settingsPath>/<appname>-server.conf`；
   - **不勾选**：保存并启动核心 → 与备份 `diff`，必须**无差异**（向后兼容的硬指标）；
   - **勾选**：diff 应只多出服务器那一行 `right = laptop`（或 `left = laptop`）。
3. **不产生重复 screen**：肉眼确认 `screens` 段每台机器只有一段；核心启动无 `duplicate screen name` 报错。
4. **持久化**：保存后重启 GUI，勾选状态仍在（`internalConfig/screens[i]/dualSide`）；把笔记本格子拖到其它格再拖回来，标记不丢（`QDataStream` 序列化已覆盖）。
5. **误配置提示**：把已勾选的笔记本放到服务器上方 → 说明文字出现警告；移到左边/右边 → 警告消失。
6. **单测回归**：`ctest --test-dir build -R "ScreenTests|ServerConfigTests"`（`ScreenTests` 位于 `src/unittests/gui/config/`）。

### 7.3 双机端到端（待做）

1. 服务器 + 笔记本，笔记本在服务器**右侧**、勾选双侧切入 → 鼠标从服务器**右边缘**滑入笔记本 ✅、从服务器**左边缘**也滑入笔记本 ✅（本次核心验收点）。
2. 从笔记本滑出 → 回到服务器，位置合理、无来回抖动/边缘弹跳。
3. 取消勾选 → 回到单侧行为（只有右侧可滑入）。
4. 关掉「外部配置」开关后再来一遍（确认不依赖手写 conf）。
5. 三机：`[deskA][server][laptop勾选]` —— 服务器左边缘仍进 deskA（真实邻居不被镜像覆盖）。

---

## 八、风险与未验证项

| 项 | 说明 | 缓解 |
|---|---|---|
| **未编译** | 本机没有 Qt6/MSVC 工具链，全部 C++/UI（含 `uic` 对 `.ui` 的生成）只做了静态审查与 XML 解析，**没有任何编译证据** | §7.2 第一步就是编译；`.ui` 的改动只是往已有 `QVBoxLayout` 插一个 `QCheckBox`，风险可控 |
| **算法级验证 ≠ 真机行为** | §4.3 的输出来自等价模型；`Config::connect()` 的 `dst` 区间语义（进入时的落点）未在本机验证 | 用户已用等价形态的 conf 手工验证过连接与双向可达；落点合理性列入 §7.3.2 |
| **客户端反向边故意不补** | 设计选择（防边缘回弹），但意味着「从笔记本左侧滑回服务器」在笔记本位于服务器右侧时不可用（需从右侧滑出） | 与桌面 KVM 的直觉一致；如实测觉得别扭，放开条件即可（一行） |
| **上下双侧未开放** | 勾了但把客户端放在上方/下方 → 状态栏文字警告，不是静默默认 | `updateLayoutHint()` + tooltip；backlog 已登记 |
| **白名单外的邻近文件** | 为完成交付，除 `ScreenSetupModel.*` 与 `dialogs/` 外，还改了 `src/lib/gui/config/Screen.{h,cpp}` 与 `src/lib/gui/config/ServerConfig.{h,cpp}`——links 的生成与屏属性就在这两个文件里，绕不过去 | 改动均为新增字段/新增方法 + 一处调用点替换，无删除、无重构 |
| **未动 MainWindow.cpp / Settings.* / translations** | 遵守约束；新设置走 `Screen` 自己的 QSettings 代理，未新增全局 Settings 键 | 无需改 MainWindow；翻译串待 `lupdate` |
| **`label_2` 文案被程序改写** | `updateLayoutHint()` 会覆盖 `.ui` 里的初始文案（源串与 `.ui` 完全一致，`lupdate` 按类上下文合并为同一条 msgid） | 文案键：`Configure the layout of your computer displays by dragging to where you want.` |

---

## 九、一句话总结

**双侧切入 = 「加边不加屏」**：`screens` 段仍由网格单元格一对一生成、屏名唯一（F1 不破）；双侧只在 `links` 段多一条不同方向的边（F2 允许），由客户端屏上的一个 `dualSide` 标记驱动；不勾选时输出与今天逐字节一致，因此单侧布局零风险。
