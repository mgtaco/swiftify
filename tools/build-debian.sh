#!/usr/bin/env bash
# build-debian.sh - Build and install swiftify on Debian/Ubuntu
#
# Handles Qt 6.8 compatibility by patching QML features that require Qt 6.9+.
# Run from the repository root or pass the repo path as an argument.
#
# Usage:
#   ./tools/build-debian.sh
#   ./tools/build-debian.sh --install-deps      # also install build dependencies
#   ./tools/build-debian.sh --prefix ~/.local   # custom install prefix

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[*]${NC} $1"; }
warn()  { echo -e "${YELLOW}[!]${NC} $1"; }
error() { echo -e "${RED}[x]${NC} $1" >&2; exit 1; }

INSTALL_DEPS=false
PREFIX="$HOME/.local"
REPO_DIR=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --install-deps    Install build dependencies (requires sudo)"
            echo "  --prefix PATH     Installation prefix (default: ~/.local)"
            echo "  -h, --help        Show this help"
            exit 0
            ;;
        *) REPO_DIR="$1"; shift ;;
    esac
done

# Find repo root
if [[ -n "$REPO_DIR" ]]; then
    cd "$REPO_DIR"
elif [[ -f "CMakeLists.txt" ]] && grep -q "project(Swiftify" CMakeLists.txt 2>/dev/null; then
    : # already in repo root
elif [[ -f "../CMakeLists.txt" ]] && grep -q "project(Swiftify" ../CMakeLists.txt 2>/dev/null; then
    cd ..
else
    error "Run this script from the swiftify repository root, or pass the path as an argument"
fi

REPO_DIR="$(pwd)"
log "Building swiftify in $REPO_DIR"

# Check for required tools
command -v cmake >/dev/null 2>&1 || error "cmake not found. Install it or run with --install-deps"
command -v ninja >/dev/null 2>&1 || error "ninja not found. Install it or run with --install-deps"
command -v rustc >/dev/null 2>&1 || error "rustc not found. Install Rust from https://rustup.rs"

# Install dependencies if requested
if $INSTALL_DEPS; then
    log "Installing build dependencies..."
    sudo apt-get update
    sudo apt-get install -y \
        git qt6-base-dev qt6-declarative-dev qt6-shader-baker qt6-shadertools-dev \
        qt6-wayland qt6-websockets-dev qtkeychain-qt6-dev qcoro-qt6-dev libpulse-dev libasound2-dev \
        libssl-dev cmake ninja-build pkg-config
fi

# Detect Qt version
QT_VERSION=$(pkg-config --modversion Qt6Core 2>/dev/null || echo "0")
QT_MAJOR=$(echo "$QT_VERSION" | cut -d. -f1)
QT_MINOR=$(echo "$QT_VERSION" | cut -d. -f2)

log "Detected Qt version: $QT_VERSION"

# Apply Qt 6.8 compatibility patches if needed
apply_qt68_patches() {
    log "Applying Qt 6.8 compatibility patches..."

    # Patch CMakeLists.txt version requirements
    sed -i 's/Qt6 6\.9/Qt6 6.8/g' CMakeLists.txt
    sed -i 's/REQUIRES 6\.9/REQUIRES 6.8/g' CMakeLists.txt

    # Remove 'final' keyword from QML properties (Qt 6.9 feature)
    find qml -name "*.qml" -exec sed -i \
        -e 's/final readonly property/readonly property/g' \
        -e 's/final property/property/g' {} \;

    # Remove ApplicationWindow padding properties (Qt 6.9 feature)
    if grep -q "bottomPadding: 0" qml/Main.qml 2>/dev/null; then
        sed -i '/topPadding: 0/d; /leftPadding: 0/d; /rightPadding: 0/d; /bottomPadding: 0/d' qml/Main.qml
        # Also remove the comment about Qt 6.9
        sed -i '/since qt 6.9/,/clear itself/d' qml/Main.qml
    fi

    log "Qt 6.8 patches applied"
}

# Check if patches are needed
if [[ "$QT_MAJOR" -eq 6 ]] && [[ "$QT_MINOR" -lt 9 ]]; then
    warn "Qt $QT_VERSION detected (< 6.9). Applying compatibility patches..."
    apply_qt68_patches
elif [[ "$QT_MAJOR" -lt 6 ]]; then
    error "Qt 6.8+ is required, but Qt $QT_VERSION was found"
fi

# Clean previous build if it exists with different Qt version
if [[ -d build ]]; then
    BUILD_QT=$(grep -o 'Qt6_VERSION "[^"]*"' build/CMakeCache.txt 2>/dev/null | cut -d'"' -f2 || echo "")
    if [[ -n "$BUILD_QT" ]] && [[ "$BUILD_QT" != "$QT_VERSION" ]]; then
        warn "Previous build used Qt $BUILD_QT, cleaning..."
        rm -rf build
    fi
fi

# Configure
log "Configuring build..."
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"

# Build
log "Building (this may take a few minutes)..."
cmake --build build

# Install
log "Installing to $PREFIX..."
cmake --install build

# Check PATH
if [[ ":$PATH:" != *":$PREFIX/bin:"* ]]; then
    warn "$PREFIX/bin is not in your PATH"
    echo ""
    echo "Add this to your ~/.bashrc or ~/.zshrc:"
    echo "  export PATH=\"$PREFIX/bin:\$PATH\""
    echo ""
fi

log "Done! Run 'swiftify' to start the app."
echo ""
echo "Before first use:"
echo "  1. You need Spotify Premium"
echo "  2. Create a Spotify app at https://developer.spotify.com/dashboard"
echo "  3. Add http://127.0.0.1:8898/login as a redirect URI"
echo "  4. Copy your Client ID (the app will ask for it)"
