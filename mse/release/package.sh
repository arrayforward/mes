#!/usr/bin/env bash
# ============================================================================
# mse/release/package.sh —— 发布包组装(可重复执行)
#
# 产物:../build/mse-release/ 目录 + ../build/mse-release.zip
# 布局:mse_server.exe / mse_demo.exe / web/ / assets/ / tools/ / *.bat /
#       README.txt / api-ms-win-crt-*.dll + ucrtbase.dll(UCRT 应用本地部署,
#       Windows SDK Redist,使老系统(无内置 UCRT)也能运行)
# 用法:bash release/package.sh   (在 mse/ 目录下,先完成 cmake --build)
# ============================================================================
set -e
cd "$(dirname "$0")/.."

BUILD=build
PKG=$BUILD/mse-release
UCRT_DIR="/c/Program Files (x86)/Windows Kits/10/Redist/ucrt/DLLs/x64"

rm -rf "$PKG" "$BUILD/mse-release.zip"
mkdir -p "$PKG"

cp "$BUILD/mse_server.exe" "$BUILD/mse_demo.exe" "$PKG/"
cp -r web "$PKG/web"
rm -f "$PKG/web/smoke.mjs" "$PKG/web/probe_backup.html"
cp -r assets "$PKG/assets"
cp -r tools "$PKG/tools"
cp release/*.bat release/README.txt "$PKG/"

# UCRT 应用本地部署(Windows SDK Redist 许可;Win10+ 实际走系统组件,
# 老系统用包内副本)。找不到 redist 时警告但不中断(Win10+ 不需要)。
if [ -d "$UCRT_DIR" ]; then
    cp "$UCRT_DIR"/api-ms-win-crt-*.dll "$UCRT_DIR/ucrtbase.dll" "$PKG/"
    echo "UCRT DLLs: $(ls "$PKG" | grep -c 'api-ms-win-crt') + ucrtbase.dll"
else
    echo "警告:未找到 Windows SDK UCRT redist($UCRT_DIR),包仅支持 Win10+"
fi

powershell -Command "Compress-Archive -Path '$PKG\*' -DestinationPath '$(pwd -W)\\$BUILD\\mse-release.zip'"
echo "打包完成:$BUILD/mse-release.zip"
ls -la "$BUILD/mse-release.zip"
