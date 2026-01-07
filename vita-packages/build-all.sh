#!/bin/bash
#
# VitaPlex - Build Vita Packages with HTTP Streaming Support
# Based on wiliwili's working PSV build configuration
#
# Prerequisites:
#   - VitaSDK installed and configured
#   - vita-makepkg available (from vitasdk)
#
# Usage:
#   ./build-all.sh          # Build all packages
#   ./build-all.sh mbedtls  # Build only mbedtls
#   ./build-all.sh ffmpeg   # Build only ffmpeg (requires mbedtls, curl)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ORDER=("mbedtls" "curl" "ffmpeg" "mpv")

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_vitasdk() {
    if [ -z "$VITASDK" ]; then
        log_error "VITASDK environment variable not set!"
        log_info "Please run: export VITASDK=/path/to/vitasdk"
        exit 1
    fi

    if [ ! -d "$VITASDK" ]; then
        log_error "VITASDK directory not found: $VITASDK"
        exit 1
    fi

    if ! command -v vita-makepkg &> /dev/null; then
        log_error "vita-makepkg not found!"
        log_info "Make sure VitaSDK bin directory is in PATH"
        exit 1
    fi

    log_success "VitaSDK found at: $VITASDK"
}

build_package() {
    local pkg_name=$1
    local pkg_dir="$SCRIPT_DIR/$pkg_name"

    if [ ! -d "$pkg_dir" ]; then
        log_error "Package directory not found: $pkg_dir"
        return 1
    fi

    if [ ! -f "$pkg_dir/VITABUILD" ]; then
        log_error "VITABUILD not found in: $pkg_dir"
        return 1
    fi

    log_info "Building package: $pkg_name"
    echo "=============================================="

    cd "$pkg_dir"

    # Clean previous build
    rm -rf src pkg *.tar.xz 2>/dev/null || true

    # Build with vita-makepkg
    if vita-makepkg; then
        log_success "Built $pkg_name successfully!"

        # Install the package
        local pkg_file=$(ls -t *.tar.xz 2>/dev/null | head -1)
        if [ -n "$pkg_file" ]; then
            log_info "Installing $pkg_file..."
            vita-makepkg -i
            log_success "Installed $pkg_name"
        fi
    else
        log_error "Failed to build $pkg_name"
        return 1
    fi

    cd "$SCRIPT_DIR"
    echo ""
}

uninstall_conflicting() {
    log_info "Checking for conflicting packages in VitaSDK..."

    # List of packages that might conflict
    local packages=("mbedtls" "curl" "ffmpeg" "mpv")

    for pkg in "${packages[@]}"; do
        # Check if package files exist in VitaSDK
        if [ -f "$VITASDK/arm-vita-eabi/lib/lib${pkg}.a" ] || \
           [ -d "$VITASDK/arm-vita-eabi/include/${pkg}" ]; then
            log_warning "Found existing $pkg installation"
            log_info "You may need to remove it first for a clean build"
        fi
    done
}

show_help() {
    echo "VitaPlex - Vita Packages Builder"
    echo ""
    echo "This script builds FFmpeg and MPV with HTTP streaming support for PS Vita."
    echo ""
    echo "Usage: $0 [package_name]"
    echo ""
    echo "Packages (build order):"
    echo "  mbedtls  - TLS/SSL library (required by curl)"
    echo "  curl     - HTTP client library (required by ffmpeg)"
    echo "  ffmpeg   - Multimedia framework with HTTP protocol support"
    echo "  mpv      - Media player with GXM rendering"
    echo ""
    echo "Options:"
    echo "  all      - Build all packages in order (default)"
    echo "  clean    - Remove all build artifacts"
    echo "  help     - Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0           # Build all packages"
    echo "  $0 ffmpeg    # Build only ffmpeg"
    echo "  $0 clean     # Clean all build directories"
}

clean_all() {
    log_info "Cleaning all build directories..."

    for pkg in "${BUILD_ORDER[@]}"; do
        local pkg_dir="$SCRIPT_DIR/$pkg"
        if [ -d "$pkg_dir" ]; then
            log_info "Cleaning $pkg..."
            rm -rf "$pkg_dir/src" "$pkg_dir/pkg" "$pkg_dir"/*.tar.xz 2>/dev/null || true
        fi
    done

    log_success "Clean complete!"
}

main() {
    echo "=============================================="
    echo "  VitaPlex - Vita Packages Builder"
    echo "  HTTP Streaming Support for PS Vita"
    echo "=============================================="
    echo ""

    local target="${1:-all}"

    case "$target" in
        help|--help|-h)
            show_help
            exit 0
            ;;
        clean)
            clean_all
            exit 0
            ;;
        all)
            check_vitasdk
            uninstall_conflicting

            log_info "Building all packages in order..."
            echo ""

            for pkg in "${BUILD_ORDER[@]}"; do
                build_package "$pkg"
            done

            log_success "All packages built successfully!"
            echo ""
            log_info "Packages installed to: $VITASDK/arm-vita-eabi"
            ;;
        mbedtls|curl|ffmpeg|mpv)
            check_vitasdk
            build_package "$target"
            ;;
        *)
            log_error "Unknown target: $target"
            show_help
            exit 1
            ;;
    esac
}

main "$@"
