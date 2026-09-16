#!/bin/bash
# build-chain2.sh — 构建链下载 v2：绕 GitHub 直连（国内网络），官方源/镜像优先
exec > /d/build-tools/downloads/build-chain2-run.log 2>&1
set -x
DL=/d/build-tools/downloads
cd "$DL"

try_download() {
  # try_download <输出文件> <url1> <url2> ... 依次尝试
  local out="$1"; shift
  for u in "$@"; do
    if curl -L --retry 2 --connect-timeout 15 --max-time 600 -o "$out" "$u"; then
      echo "[OK] $out <- $u $(date '+%F %T')"
      return 0
    fi
    echo "[RETRY] $out failed from $u"
  done
  return 1
}

# 1) CMake：官方 cmake.org（自带托管不走 github）+ 镜像兜底
if try_download cmake.zip \
    "https://cmake.org/files/v4.4/cmake-4.4.2-windows-x86_64.zip" \
    "https://ghproxy.net/https://github.com/Kitware/CMake/releases/download/v4.4.2/cmake-4.4.2-windows-x86_64.zip"; then
  curl -L --connect-timeout 15 -o cmake-SHA256.txt "https://ghproxy.net/https://github.com/Kitware/CMake/releases/download/v4.4.2/SHA256.txt" || true
  if [ -s cmake-SHA256.txt ] && grep "cmake-4.4.2-windows-x86_64.zip" cmake-SHA256.txt | sha256sum -c -; then
    echo "| cmake zip | 官方源 | SHA256 与官方清单一致 | ✅ |" >> /d/projects/litekvm-design/security-log.md
    unzip -q -o cmake.zip -d /d/build-tools/
    /d/build-tools/cmake-4.4.2-windows-x86_64/bin/cmake.exe --version && echo "CMAKE_OK"
  else
    echo "| cmake zip | 官方源 | 清单下载失败,本地哈希=$(sha256sum cmake.zip | cut -c1-16)... | ⚠️待复核 |" >> /d/projects/litekvm-design/security-log.md
    unzip -q -o cmake.zip -d /d/build-tools/ 2>/dev/null && /d/build-tools/cmake-4.4.2-windows-x86_64/bin/cmake.exe --version && echo "CMAKE_OK_UNVERIFIED"
  fi
fi

# 2) Ninja：ghproxy 镜像 + 直连兜底
if try_download ninja-win.zip \
    "https://ghproxy.net/https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip" \
    "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip"; then
  mkdir -p /d/build-tools/ninja
  unzip -q -o ninja-win.zip -d /d/build-tools/ninja
  NV=$(/d/build-tools/ninja/ninja.exe --version) && echo "| ninja v$NV | 官方release(镜像) | 本地哈希=$(sha256sum ninja-win.zip | cut -c1-16)... | ⚠️人工复核 |" >> /d/projects/litekvm-design/security-log.md && echo "NINJA_OK"
fi

echo "PHASE_A_DONE $(date '+%F %T')"
