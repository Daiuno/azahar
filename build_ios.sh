#!/bin/bash
# Azahar iOS LibRetro 构建脚本

set -e  # 出错时退出

echo "================================================"
echo "  Azahar iOS LibRetro Core Build Script"
echo "================================================"

# 清理旧构建
# echo "清理旧构建目录..."
# rm -rf build/ios-arm64

# 配置CMake
echo "配置CMake..."
cmake \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_C_FLAGS=-DIOS \
  -DCMAKE_CXX_FLAGS=-DIOS \
  -DIOS=ON \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCITRA_USE_PRECOMPILED_HEADERS=OFF \
  -DENABLE_OPT=OFF \
  -DENABLE_LIBRETRO=ON \
  -DENABLE_SDL2=OFF \
  -DENABLE_QT=OFF \
  -DENABLE_TESTS=OFF \
  -DENABLE_ROOM=OFF \
  -DENABLE_WEB_SERVICE=OFF \
  -DENABLE_SCRIPTING=OFF \
  -DENABLE_CUBEB=OFF \
  -DENABLE_OPENAL=ON \
  -DENABLE_LIBUSB=OFF \
  -DCITRA_WARNINGS_AS_ERRORS=OFF \
  -DENABLE_OPENGL=OFF \
  -DENABLE_VULKAN=ON \
  . -B build/ios-arm64

# 构建
echo "开始构建..."
CPU_COUNT=$(sysctl -n hw.ncpu)
echo "使用 $CPU_COUNT 个CPU核心并行构建"
cmake --build build/ios-arm64 --target azahar_libretro --config Release -j$CPU_COUNT

# 验证
if [ -d "build/ios-arm64/azahar.libretro.framework" ]; then
    echo ""
    echo "================================================"
    echo "✅ 构建成功!"
    echo "================================================"
    echo "输出文件: build/ios-arm64/azahar.libretro.framework"
    echo ""
    echo "Framework 信息:"
    ls -lh build/ios-arm64/azahar.libretro.framework/azahar.libretro
    echo ""
    echo "架构信息:"
    file build/ios-arm64/azahar.libretro.framework/azahar.libretro
    echo ""
    echo "符号检查:"
    nm -g build/ios-arm64/azahar.libretro.framework/azahar.libretro | grep "retro_" | head -5
    echo "..."
    echo ""
    echo "下一步:"
    echo "1. 将 azahar.libretro.framework 复制到 RetroArch 的 frameworks 目录"
    echo "2. 确保 aes_keys.txt 在正确位置"
    echo "3. 在 RetroArch 中加载 Azahar 核心"
else
    echo ""
    echo "================================================"
    echo "❌ 构建失败!"
    echo "================================================"
    echo "请检查上面的错误信息"
    exit 1
fi



