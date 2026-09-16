# 移除跨网中继功能（litekvm-pro）

## 决策
用户决定砍掉跨网中继（VPS relay + 中继地址/token 配置），litekvm-pro 聚焦：
1. ✅ 保留：开机自启 + 自动连接已配对设备
2. ✅ 保留：多机共享键鼠（mesh 角色协商 + 布局同步）
3. ❌ 移除：跨网互联（relay/ 目录 + RelayLink 模块 + ProSettingsDialog 中继配置）

relay 代码保留在 git 历史（74f1cb7a4），需要时随时回捞。

## 清理清单
- [ ] relay/ 目录整个删除（git rm -r relay/）
- [ ] src/lib/litekvm/RelayLink.h/.cpp 删除 + CMakeLists 移除条目
- [ ] ProSettingsDialog 移除中继地址/房间令牌两个输入框（保留自启/自动连复选框）
- [ ] docs/relay-deploy.md 删除
- [ ] README.md 移除"自部署中继"特性行
- [ ] LiteKvmController 检查有无 relay 引用，清理
- [ ] 全量构建 + 测试回归
- [ ] commit + push origin litekvm/pro
- [ ] 重打桌面 zip（LiteKVM-Pro-v2.0-norelay.zip）
