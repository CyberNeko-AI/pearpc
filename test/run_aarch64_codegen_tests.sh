#!/bin/bash
# Run after a normal macOS AArch64 JIT build, from any working directory.
set -euo pipefail

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
    echo "SKIP: native codegen tests require macOS arm64"
    exit 0
fi

cd "$(dirname "$0")/.."
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/pearpc-codegen.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
cpu_dir=src/cpu/cpu_jitc_aarch64

# Dead stripping keeps the actual emitter and register-flush implementation
# while excluding unused emulator entry points and device dependencies.
"${CXX:-c++}" -std=c++11 -DHAVE_CONFIG_H -I. -Isrc -Wl,-dead_strip \
    test/test_stwcx_fragments.cc "$cpu_dir/jitc.o" "$cpu_dir/ppc_mmu.o" \
    "$cpu_dir/ppc_alu.o" "$cpu_dir/aarch64asm.o" -o "$test_dir/test_stwcx_fragments"
"$test_dir/test_stwcx_fragments"
