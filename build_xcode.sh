#!/bin/bash
set -euo pipefail

# ── Configuration ──────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/ios-arm64-xcode"
DEPLOYMENT_TARGET="14.0"

# ── Step 1: Init submodules ───────────────────────────────────────────────────
echo "==> Checking submodules..."
cd "$SCRIPT_DIR"
if [ -z "$(ls -A externals/fmt 2>/dev/null)" ]; then
    echo "    Initializing submodules (first run may take a while)..."
    git submodule update --init --recursive
else
    echo "    Submodules already initialized."
fi

# ── Step 2: Create CMake injection script ─────────────────────────────────────
# Injects FRAMEWORK properties into azahar_libretro target without modifying
# the project's own CMakeLists.txt.
# OUTPUT_NAME changes the product from azahar_libretro.framework → azahar.libretro.framework
INJECT_CMAKE="${BUILD_DIR}/_framework_inject.cmake"
mkdir -p "$BUILD_DIR"
cat > "$INJECT_CMAKE" << 'CMAKE'
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
    set_target_properties azahar_libretro PROPERTIES
        FRAMEWORK TRUE
        FRAMEWORK_VERSION A
        OUTPUT_NAME "azahar.libretro"
        MACOSX_FRAMEWORK_IDENTIFIER com.azahar.libretro
        MACOSX_FRAMEWORK_BUNDLE_VERSION "1.0.0"
        MACOSX_FRAMEWORK_SHORT_VERSION_STRING "1.0.0"
        XCODE_ATTRIBUTE_SKIP_INSTALL "YES"
        XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL "s"
)
CMAKE

# ── Step 3: CMake configure with Xcode generator ─────────────────────────────
echo "==> Configuring CMake with Xcode generator for iOS arm64..."
cmake \
    -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS="-DIOS" \
    -DCMAKE_CXX_FLAGS="-DIOS" \
    -DIOS=ON \
    -DENABLE_LIBRETRO=ON \
    -DCITRA_USE_PRECOMPILED_HEADERS=OFF \
    -DENABLE_OPT=OFF \
    -DCMAKE_PROJECT_INCLUDE="${INJECT_CMAKE}" \
    -S "$SCRIPT_DIR" \
    -B "$BUILD_DIR"

# ── Step 4: Post-process — set defaultConfigurationName to Release ──────────
# CMake sets defaultConfigurationName = Debug, which causes sub-project builds
# to fall back to Debug when the parent uses a custom configuration name
# (e.g. SideloadRelease, DevRelease). Changing this to Release ensures the
# fallback uses optimized settings.
echo "==> Patching defaultConfigurationName to Release..."
sed -i '' 's/defaultConfigurationName = Debug/defaultConfigurationName = Release/g' \
    "$BUILD_DIR"/azahar.xcodeproj/project.pbxproj

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo "==> Done! Xcode project generated."
echo ""
echo "    Open with:"
echo "    open ${BUILD_DIR}/citra.xcodeproj"
echo ""
echo "    In Xcode:"
echo "      - Select scheme: azahar_libretro"
echo "      - Select destination: Any iOS Device (arm64)"
echo "      - Build (Cmd+B)"
echo "      - Product: azahar.libretro.framework"
