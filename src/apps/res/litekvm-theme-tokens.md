# LiteKVM 设计 Token — 深色主题

> 适用文件：`src/apps/res/litekvm-theme-dark.qss`（样式表）
> 加载入口：`deskflow::gui::applyLiteKvmTheme()`（`src/lib/gui/ThemeLoader.cpp`，声明在 `ThemeLoader.h`，由 `StyleUtils.h` 转出）
> 风格取向：**Linear / Vercel 式现代深色控制台**——扁平分层表面、高对比正文、6–8px 圆角、单一克制的靛蓝强调色，无渐变、无插画、无发光。

## 1. 为什么选这套风格

| 取向 | 理由 |
| --- | --- |
| 近黑画布 `#0D0F12` 而非纯黑 | 纯黑 `#000` 在 LCD 上边缘发死、卡片分不出层；Linear/Vercel 都用「近黑 + 三级灰面」来制造层次 |
| 三级表面（canvas / surface / sunken） | LiteKVM 是「状态 + 列表 + 日志」型工具界面，用户视线要能一眼区分「窗口底 / 卡片 / 输入与日志区」；不靠阴影靠亮度分层，缩放/DPI 下更稳 |
| 单一强调色靛蓝 `#5E6AD2` | 一个应用只需要一个「主操作色」（开始/停止、默认按钮）。避免多色竞争；靛蓝在近黑上对比高又不像纯蓝那样刺眼，且是 Linear 一脉的既有语汇 |
| 16px 圆角用 6px、卡片用 8px | 圆角与控件尺寸成比例：32px 高按钮用 6px，卡片/列表用 8px。过大的圆角（12px+）在 32px 控件上会显得「手机 App」而非桌面控制台 |
| 文本不纯白 `#E7EAEE` | 纯白在暗底上会「发光」并凸显次像素锯齿；`#E7EAEE` 保留 15.9:1 对比度的同时更耐看 |
| 中文优先字体栈 | 主力用户是中文 Windows，字体栈第一顺位就是 `Microsoft YaHei UI`，避免落到 Segoe UI 后缺字回退到点阵宋体 |

## 2. 颜色 Token

### 2.1 表面 / 层次

| Token | 值 | 用途 |
| --- | --- | --- |
| `bg/canvas` | `#0D0F12` | 窗口底色、菜单栏、状态栏、dock 标题栏 |
| `bg/surface` | `#14171C` | 卡片（卡片式 QGroupBox）、菜单、弹层、表头、消息框 |
| `bg/sunken` | `#0A0C0E` | 输入框、下拉框、日志面板、表格/列表视图（凹槽感） |
| `bg/row-alt` | `#101317` | 表格交替行（`alternate-background-color`，需 `setAlternatingRowColors(true)`） |
| `bg/hover` | `#1B2027` | 悬停填充（行、菜单项、扁平按钮、工具提示底色） |
| `bg/active` | `#232A33` | 按下填充、扁平按钮选中 |
| `bg/readonly` | `#101317` | 只读输入框 |

### 2.2 描边

| Token | 值 | 用途 |
| --- | --- | --- |
| `border/subtle` | `#1F242B` | 卡片描边、分隔线（dock 标题上边线、菜单分隔、表格网格线、滚动条槽） |
| `border/default` | `#2A3038` | 控件描边（按钮、输入框、下拉框未聚焦态）、滚动条滑块 |
| `border/strong` | `#3A424C` | 悬停态描边、复选框/单选框未选中描边、滚动条滑块悬停 |

### 2.3 文本

| Token | 值 | 用途 |
| --- | --- | --- |
| `text/primary` | `#E7EAEE` | 正文、标题、表格内容 |
| `text/secondary` | `#A2ABB8` | 说明文字、表头、状态栏、菜单栏、dock 标题 |
| `text/muted` | `#7A8494` | IP 地址、辅助元信息（小号） |
| `text/log` | `#C9D1D9` | 日志正文（比正文略冷，长时间阅读更舒适） |
| `text/disabled` | `#6E7783` | 禁用态文本 |
| `text/inverse` | `#FFFFFF` | 强调色之上的文字（主按钮、选中行） |

### 2.4 强调与语义色

| Token | 值 | 用途 |
| --- | --- | --- |
| `accent/default` | `#5E6AD2` | 主操作按钮底、默认按钮底、聚焦描边、复选框选中、单选框选中环 |
| `accent/hover` | `#636DD4` | 主操作按钮悬停（刻意只提亮一档，见 2.6 对比度） |
| `accent/pressed` | `#4C57BF` | 主操作按钮按下 |
| `accent/border` | `#6E79DB` | 主操作按钮描边（比底色亮一档，制造「凸起」边缘） |
| `accent/focus` | `#8B95F0` | 聚焦态描边（仅键盘聚焦） |
| `accent/tint` | `#1E2440` | 选中的模式卡片底色 |
| `accent/selection` | `#232A4D` | 列表/表格/菜单选中行底色 |
| `accent/text` | `#8B95F0` / `#9AA3F5` | 需要以文字呈现强调色时（画布上 / 选中底上） |
| `semantic/success` | `#3FB950` | 已配对、连接成功（配对成功提示文案） |
| `semantic/warning` | `#D29922` | `lblNoMode`「必须选择模式」 |
| `semantic/danger` | `#E5484D` | 配对失败、错误状态 |

### 2.5 间距 / 圆角 / 字号

| Token | 值 | 说明 |
| --- | --- | --- |
| 间距基数 | 2 / 4 / 6 / 8 / 12 / 16 / 20 / 24 px | 4px 网格，QSS 里体现为 `padding` / `margin` / `spacing` |
| 控件内边距 | `4px 14px`（按钮）、`0 8px`（输入框，无纵向内边距）、`6px 8px`（表格单元格/日志） | 输入框不用纵向 padding：日志 dock 内搜索框在 C++ 里被 `setMaximumHeight()` 卡在 ~20px，纵向 padding 会裁字 |
| 圆角 | `radius/sm 4px`（复选框）、`radius/md 6px`（按钮、输入框、菜单项、滚动条滑块）、`radius/lg 8px`（卡片、列表、表格、菜单、模式卡片）、`radius/full 8px`（单选框 16px 直径） | |
| 字体栈（UI） | `"Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", "Inter", "Noto Sans CJK SC", "Source Han Sans SC", sans-serif` | 中文优先回退链；同时由 `ThemeLoader` 通过 `QFont::setFamilies()` 设进 `QApplication`，让 QSS 未覆盖的控件也能正确显示中文 |
| 字体栈（等宽） | `"Cascadia Mono", "Consolas", "Hack", "Microsoft YaHei UI", monospace` | 仅日志面板（覆盖 `LogWidget.cpp` 里 `setFont(fixedFont())` 设的 Hack；Cascadia Mono / Consolas 在 Windows 上更常驻） |
| 字号 | `13px` 正文 / `12px` 次要（表头、状态栏、日志、tooltip、dock 标题、IP）/ `15px` 计算机名标题 | 13px 是 Windows 100% 缩放下接近系统默认又略大的控制台字号 |
| 字重 | `400` 正文 / `500` 按钮 / `600` 标题、主按钮、表头、选中模式卡片 | 用字重而非颜色做层级，减少颜色数量 |

### 2.6 对比度核验（WCAG 2.1，实算值）

| 组合 | 对比度 |
| --- | --- |
| `text/primary` on canvas | 15.90 : 1 |
| `text/primary` on surface | 14.88 : 1 |
| `text/primary` on sunken | 16.24 : 1 |
| `text/secondary` on canvas | 8.27 : 1 |
| `text/secondary` on surface | 7.74 : 1 |
| `text/muted` on canvas | 5.08 : 1 |
| `text/muted` on surface | 4.75 : 1 |
| `text/log` on sunken | 12.69 : 1 |
| `#FFFFFF` on `accent/default` `#5E6AD2` | 4.70 : 1 |
| `#FFFFFF` on `accent/hover` `#636DD4` | 4.50 : 1 |
| `#FFFFFF` on `accent/pressed` `#4C57BF` | 6.14 : 1 |
| `#FFFFFF` on 选中行 `#232A4D` | 13.91 : 1 |
| `accent/ring` `#5E6AD2` vs canvas（非文本） | 4.08 : 1 |
| `semantic/success` on surface | 7.07 : 1 |
| `semantic/warning` on surface | 7.12 : 1 |
| `semantic/danger` on surface | 4.59 : 1 |
| `text/disabled` `#6E7783` on surface | 3.96 : 1（禁用态，豁免 AA） |

> `accent/hover` 之所以定在 `#636DD4` 而不是更亮的 `#6E79DB`，是因为更亮的版本配白字只有 3.87:1，低于 AA 4.5:1。主按钮悬停靠 **底色一档提亮 + 描边提亮** 双重反馈，视觉上依然明显。

## 3. 组件配方（QSS 里怎么落地）

| 组件 | 配方 |
| --- | --- |
| 主操作按钮 `#btnToggleCore`（开始/停止） | 靛蓝实底 + 600 字重 + 白字 + 6px 圆角；`:disabled` 回落为 `bg/hover` 底 + 禁用字色（该按钮在 .ui 里默认 disabled） |
| 对话框默认按钮 `QPushButton:default` | 同主操作配方（确定/连接类操作自动获得强调） |
| 次级按钮（`#btnConfigureServer/Client`） | `bg/hover` 底 + `border/default` 描边 + 正常字色 |
| 扁平图标按钮 `QPushButton[flat="true"]` | 透明底、无描边、`padding: 0`（C++ 里是 `setFixedSize(图标+2px)`，有 padding 会裁图标）；`:hover` 上 `bg/hover` |
| 32×32 图标按钮（`#btnSaveServerConfig` `#btnRestartCore` `#btnEditName`） | 同上，按 objectName 单独保证 `padding: 0` |
| 模式卡片（`#rbModeServer` `#rbModeClient`） | 未选中 = surface 底 + subtle 描边；`:checked` = `accent/tint` 底 + 靛蓝描边；整块可点，线性（Linear）式单选卡片 |
| 复选/单选框指示器 | 复选框：16px 圆角方块，选中填靛蓝；单选框：16px 圆，选中为 5px 靛蓝粗边环（QSS 无法画「点」，粗边环是纯 QSS 能做到的最清晰选中态） |
| 列表/表格/近邻电脑列表 | `bg/sunken` 底 + `border/subtle` 描边 + 8px 圆角卡片；选中行 `accent/selection`；表头 surface 底、600 字重 12px |
| 日志面板 | QPlainTextEdit 用 `bg/sunken` + 等宽 12px + 6px 内边距，读起来像终端；状态栏 12px 次要色 + 顶部 1px 分隔 |
| 菜单 | 8px 圆角浮层 + 6px 内边距 + 菜单项 6px 圆角，选中项用 `accent/selection` 而非系统高亮 |
| 滚动条 | 12px 无槽透明轨道 + 圆角滑块 + 悬停提亮，去掉 `add-line/sub-line`（不画箭头） |

## 4. 已知取舍（有意为之）

1. **必须配一份深色 `QPalette`**（在 `ThemeLoader.cpp` 里，约 20 行）。原因：QSS 改不了由 `QStyle` 亲手绘制的子控件字形——下拉框箭头、上下调节按钮箭头、`QProgressBar` 文本等。若只上 QSS 而不换调色板，浅色系统主题下这些箭头会按浅色绘制，出现在深色控件上即「看不见」。策略是：**可见外观全走 QSS，调色板只兜住 QSS 够不到的字形**。
2. **不写 `QComboBox::drop-down` / `::down-arrow` / `QSpinBox::up-button` 规则**：一旦给这些子控件写了 box 规则却没有 `image:`，Qt 会连原生箭头一起不画。所以 QSS 只给它们留出 `padding-right` 空间，箭头交给风格 + 深色调色板。
3. **复选框选中态用实心靛蓝方块，不是勾**：QSS 无法绘制矢量勾，`image:` 又需要一个新 SVG 资源（本任务白名单不含新增图标资源）。后续要升级：在 `src/apps/res/icons/` 加一个 `check.svg`、登记进 `deskflow.qrc`，然后在 QSS 里补一行 `QCheckBox::indicator:checked { image: url(:/icons/.../check.svg); }`。
4. **图标主题强制为 `deskflow-dark`**：应用现在是「永远深色」，而 `StyleUtils.h::updateIconTheme()` 是按系统明暗挑 `deskflow-dark/light` 的。系统若是浅色，浅色图标（深色字形）落在深色 UI 上会糊掉，所以 `ThemeLoader` 在 `updateIconTheme()` 之后把图标主题重设为 `-dark`（环境变量 `LITEKVM_DISABLE_THEME=1` 可整体关掉主题）。
5. **`NearbyPanel` 标题的自加粗样式保留**（`title->setStyleSheet("font-weight: bold;")` 是内联样式，优先级高于本 QSS）；面板内其他文字由 `QWidget#nearbyPanel QLabel` 统一为次要色。
6. **不给 `QLabel` / 基础 `QWidget` 规则加 `background`**：`validators/ValidationError.cpp` 的错误提示条是靠 `QPalette::Window = crimson` + `autoFillBackground(true)` 自绘的，QSS 一旦给 `QLabel` 写背景规则就会顶掉那条红底。基础 `QWidget` 规则因此只放字体与文字色。
7. **`MainWindow.ui` 里被固定的尺寸没动**：`widget` 高 28px、按钮 32×32、`serverOptions` 32px 等，所以按钮/输入框的内边距都按「塞得进这些固定高度」反推（按钮 `4px 14px`、输入框纵向 0）。13px 字号下按钮约 28px、输入框约 20px，均在限高内留有余量。

## 5. 改样式怎么改

- 只改颜色/圆角/间距 → 直接改 `litekvm-theme-dark.qss`，并同步本文件第 2 节表格。
- 新增控件类型 → 在 QSS 对应分区（1 基础 / 2 菜单 / 3 按钮 / 4 输入 / 5 选择 / 6 容器 / 7 视图 / 8 状态栏 / 9 主窗口 / 10 对话框）加规则，**不要**在末尾堆「补丁式」规则。
- 调试热加载：设 `LITEKVM_THEME_QSS=<绝对路径>` 指向磁盘上的 qss，`ThemeLoader` 会优先加载它（省去重编译）。
- 关闭主题：`LITEKVM_DISABLE_THEME=1`。
