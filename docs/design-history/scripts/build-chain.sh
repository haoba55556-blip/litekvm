#!/bin/bash
# build-chain.sh — 补齐构建链下载与解压（官方源+SHA256校验，零安装器，无UAC）
exec > /d/build-tools/downloads/build-chain-run.log 2>&1
set -x
DL=/d/build-tools/downloads
cd "$DL"

# 1) CMake 便携版（重新下完整包）
curl -L --retry 3 --connect-timeout 30 -o cmake.zip https://github.com/Kitware/CMake/releases/download/v4.4.2/cmake-4.4.2-windows-x86_64.zip
curl -L --retry 3 --connect-timeout 30 -o cmake-SHA256.txt https://github.com/Kitware/CMake/releases/download/v4.4.2/SHA256.txt
if grep "cmake-4.4.2-windows-x86_64.zip" cmake-SHA256.txt | sha256sum -c -; then
  echo "[OK] cmake sha256 verified $(date '+%F %T')" >> /d/projects/litekvm-design/security-log.md
else
  echo "[FAIL] cmake sha256 MISMATCH $(date '+%F %T')" >> /d/projects/litekvm-design/security-log.md
  exit 1
fi
unzip -q -o cmake.zip -d /d/build-tools/
/d/build-tools/cmake-4.4.2-windows-x86_64/bin/cmake.exe --version || exit 1
echo "| cmake-4.4.2-windows-x86_64.zip | github.com/Kitware/CMake v4.4.2 | 见SHA256.txt核对通过 | ✅ |" >> /d/projects/litekvm-design/security-log.md

# 2) Ninja
curl -L --retry 3 --connect-timeout 30 -o ninja-win.zip https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip
mkdir -p /d/build-tools/ninja
unzip -q -o ninja-win.zip -d /d/build-tools/ninja
NINJA_VER=$(/d/build-tools/ninja/ninja.exe --version) || exit 1
echo "| ninja-win.zip v$NINJA_VER | github.com/ninja-build v1.13.2 | 无官方清单，本地哈希: $(sha256sum ninja-win.zip | cut -c1-16)... | ⚠️人工复核 |" >> /d/projects/litekvm-design/security-log.md

# 3) VS2022 bootstrapper 仅预下载（绝不执行——需 UAC）
curl -L --retry 3 --connect-timeout 30 -o /d/build-tools/vs_Community.exe https://aka.ms/vs/17/release/vs_Community.exe
VS_SHA=$(sha256sum /d/build-tools/vs_Community.exe | awk '{print $1}')
echo "| vs_Community.exe (bootstrapper, 未执行) | aka.ms/vs/17/release | $VS_SHA | ⚠️待用户回来执行安装命令 |" >> /d/projects/litekvm-design/security-log.md

echo "ALL_DOWNLOADS_DONE $(date '+%F %T')"
