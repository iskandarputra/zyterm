#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build.sh — build, format, lint, test, and package zyterm
#
# Author:    Iskandar Putra (www.iskandarputra.com)
# Copyright: Copyright (c) 2026 Iskandar Putra. All rights reserved.
# License:   MIT — see LICENSE for details.
#
# Usage:
#   ./build.sh build          compile release binary
#   ./build.sh debug          compile debug binary (-O0 -g3)
#   ./build.sh format         auto-format all source files (clang-format)
#   ./build.sh format-check   check formatting without modifying files
#   ./build.sh lint           run cppcheck static analysis
#   ./build.sh test           build + run the full test suite
#   ./build.sh deb            build + package as .deb
#   ./build.sh install        build + package .deb + install via dpkg
#   ./build.sh clean          remove build artifacts + .deb files
#   ./build.sh all            format + lint + build + test + deb
#   ./build.sh help           show this help message
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── Helpers ──────────────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

info()  { printf "${CYAN}▸${RESET} %s\n" "$*"; }
ok()    { printf "${GREEN}✔${RESET} %s\n" "$*"; }
warn()  { printf "${YELLOW}⚠${RESET} %s\n" "$*"; }
die()   { printf "${RED}✖${RESET} %s\n" "$*" >&2; exit 1; }

# Extract version from src/zt_ctx.h
get_version() {
    grep -oP '#define\s+ZT_VERSION\s+"\K[^"]+' src/zt_ctx.h \
        || die "Cannot extract ZT_VERSION from src/zt_ctx.h"
}

# ── Commands ─────────────────────────────────────────────────────────────────

cmd_build() {
    info "Building zyterm (release)..."
    make -j"$(nproc 2>/dev/null || echo 4)"
    ok "Build complete: ./zyterm ($(du -h zyterm | cut -f1) stripped)"
}

cmd_debug() {
    info "Building zyterm (debug)..."
    make debug -j"$(nproc 2>/dev/null || echo 4)"
    ok "Debug build complete: ./zyterm"
}

cmd_format() {
    if ! command -v clang-format &>/dev/null; then
        die "clang-format not found. Install with: sudo apt install clang-format"
    fi
    info "Formatting source files..."
    find src include -name '*.[ch]' -print0 | xargs -0 clang-format -i
    ok "All source files formatted"
}

cmd_format_check() {
    if ! command -v clang-format &>/dev/null; then
        die "clang-format not found. Install with: sudo apt install clang-format"
    fi
    info "Checking source formatting..."
    local bad_files
    bad_files=$(find src include -name '*.[ch]' -print0 \
        | xargs -0 clang-format --dry-run --Werror 2>&1 || true)
    if [ -n "$bad_files" ]; then
        echo "$bad_files"
        die "Formatting issues found. Run: ./build.sh format"
    fi
    ok "All source files are correctly formatted"
}

cmd_lint() {
    if ! command -v cppcheck &>/dev/null; then
        die "cppcheck not found. Install with: sudo apt install cppcheck"
    fi
    info "Running cppcheck..."
    cppcheck --enable=all --quiet \
        --suppress=missingIncludeSystem \
        -Iinclude -Isrc src/
    ok "Lint complete"
}

cmd_test() {
    info "Building embed archive + running tests..."
    make zyterm_embed.a
    make test
    ok "All tests passed"
}

cmd_deb() {
    local version arch pkg_name deb_root bin_dir

    version="$(get_version)"
    arch="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
    pkg_name="zyterm_${version}_${arch}"
    deb_root="build/deb/${pkg_name}"

    info "Packaging zyterm ${version} (${arch}) as .deb..."

    # Build release binary first
    make -j"$(nproc 2>/dev/null || echo 4)"

    # Clean previous packaging
    rm -rf "build/deb"
    mkdir -p "${deb_root}/DEBIAN"
    mkdir -p "${deb_root}/usr/local/bin"

    # Install binary
    install -m 755 zyterm "${deb_root}/usr/local/bin/zyterm"

    # Calculate installed size (in KiB, as dpkg expects)
    local installed_size
    installed_size=$(du -sk "${deb_root}/usr" | cut -f1)

    # Generate control file
    cat > "${deb_root}/DEBIAN/control" <<EOF
Package: zyterm
Version: ${version}
Architecture: ${arch}
Maintainer: Iskandar Putra <iskandar@users.noreply.github.com>
Installed-Size: ${installed_size}
Depends: libc6
Description: Zero-dependency high-performance serial terminal
 A friendly serial terminal for talking to microcontrollers and embedded
 boards. Features scrollback, search, log capture, hex view, watch-pattern
 highlights, macros, fuzzy command finder, and a live-updating HUD.
 .
 One small binary. No runtime dependencies beyond libc. Built for embedded
 developers who need a fast, reliable serial monitor for RTOS targets.
Homepage: https://github.com/iskandarputra/zyterm
Section: comm
Priority: optional
EOF

    # Build the .deb
    mkdir -p releases
    dpkg-deb --build --root-owner-group "${deb_root}" "releases/${pkg_name}.deb"

    ok "Package created: releases/${pkg_name}.deb"
    info "Inspect with:  dpkg-deb -I releases/${pkg_name}.deb"
    info "Contents:      dpkg-deb -c releases/${pkg_name}.deb"
}

cmd_install() {
    cmd_deb
    local version arch pkg_name
    version="$(get_version)"
    arch="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
    pkg_name="zyterm_${version}_${arch}"

    info "Installing releases/${pkg_name}.deb..."
    sudo dpkg -i "releases/${pkg_name}.deb"
    ok "zyterm ${version} installed — run: zyterm --version"
}

cmd_clean() {
    info "Cleaning build artifacts..."
    make clean
    rm -rf build/deb releases
    ok "Clean complete"
}

cmd_all() {
    cmd_format
    cmd_lint
    cmd_build
    cmd_test
    cmd_deb
}

cmd_help() {
    cat <<'EOF'
zyterm build helper

Usage: ./build.sh <command>

Commands:
  build          Compile release binary (./zyterm)
  debug          Compile debug binary (-O0 -g3 -DZT_DEBUG)
  format         Auto-format all C source files (clang-format)
  format-check   Check formatting without modifying (for CI)
  lint           Run cppcheck static analysis
  test           Build + run full test suite
  deb            Build + package as .deb (output: releases/)
  install        Build + package .deb + install via dpkg
  clean          Remove all build artifacts and packages
  all            format → lint → build → test → deb
  help           Show this message

Examples:
  ./build.sh build                    # quick release build
  ./build.sh deb                      # produce releases/zyterm_X.Y.Z_amd64.deb
  ./build.sh install                  # build, package, and install
  ./build.sh all                      # full pipeline
EOF
}

# ── Dispatch ─────────────────────────────────────────────────────────────────

case "${1:-help}" in
    build)        cmd_build ;;
    debug)        cmd_debug ;;
    format)       cmd_format ;;
    format-check) cmd_format_check ;;
    lint)         cmd_lint ;;
    test)         cmd_test ;;
    deb)          cmd_deb ;;
    install)      cmd_install ;;
    clean)        cmd_clean ;;
    all)          cmd_all ;;
    help|--help|-h) cmd_help ;;
    *)
        die "Unknown command: $1 (run ./build.sh help)"
        ;;
esac
