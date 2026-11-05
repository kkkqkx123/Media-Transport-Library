#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation
#
# Cross-build Media-Transport-Library and the rk3588 apps for RK3588 (aarch64)
# on an x86_64 host. Wraps the verified manual procedure documented in
# docs/dev/02-x86到RK3588交叉编译分析.md and docs/dev/compile/5.10-debian-cross-compile.md.
#
# Usage:
#   script/build_rk3588_cross.sh [options]
#
# Options:
#   --sysroot PATH        aarch64 sysroot (default: auto-detect)
#   --toolchain PATH      ARM GNU Toolchain root (default: auto-detect)
#   --stages LIST         comma list: dpdk,xdp,mtl,manager,apps (default: all)
#   --buildtype TYPE      release|debug|debugonly|plain (default: release)
#   --jobs N              parallel jobs (default: nproc)
#   --clean               remove build dirs before building
#   --fetch               download DPDK/xdp-tools sources if missing
#   --no-xdp              skip AF_XDP backend (no xdp-tools/libxdp)
#   --no-apps-rx          build file_playout TX only (no RX/MPP/DRM/ALSA)
#   --usdt                enable USDT (requires sys/sdt.h in sysroot)
#   --patch-stdbool       append #include <stdbool.h> to app .c files missing it
#   -h, --help            show this help
#
# Environment overrides: RK3588_SYSROOT, RK3588_TOOLCHAIN, RK3588_STAGES,
# RK3588_BUILDTYPE, RK3588_JOBS, RK3588_ENABLE_XDP, RK3588_ENABLE_USDT.

set -euo pipefail

script_path=$(readlink -f "${BASH_SOURCE[0]}")
script_dir=$(dirname "$script_path")
repo_dir=$(readlink -f "$script_dir/..")
# shellcheck disable=SC1091
. "$script_dir/common.sh"

# ---------- defaults ----------
SYSROOT="${RK3588_SYSROOT:-}"
TOOLCHAIN="${RK3588_TOOLCHAIN:-}"
STAGES="${RK3588_STAGES:-all}"
BUILDTYPE="${RK3588_BUILDTYPE:-release}"
JOBS="${RK3588_JOBS:-$(nproc)}"
CLEAN=0
FETCH=0
ENABLE_XDP="${RK3588_ENABLE_XDP:-1}"
ENABLE_USDT="${RK3588_ENABLE_USDT:-0}"
MP4_TX=1
MP4_RX=1
PATCH_BOOL=0

show_help() {
	cat <<EOF
Usage: $(basename "$0") [options]

Cross-build MTL + rk3588 apps for RK3588 (aarch64).

Options:
  --sysroot PATH        aarch64 sysroot (default: auto-detect)
  --toolchain PATH      ARM GNU Toolchain root (default: auto-detect)
  --stages LIST         comma list: dpdk,xdp,mtl,manager,apps (default: all)
  --buildtype TYPE      release|debug|debugonly|plain (default: release)
  --jobs N              parallel jobs (default: nproc)
  --clean               remove build dirs before building
  --fetch               download DPDK/xdp-tools sources if missing
  --no-xdp              skip AF_XDP backend
  --no-apps-rx          build file_playout TX only
  --usdt                enable USDT (requires sys/sdt.h)
  --patch-stdbool       append #include <stdbool.h> to app .c missing it
  -h, --help            show this help

Environment: RK3588_SYSROOT RK3588_TOOLCHAIN RK3588_STAGES RK3588_BUILDTYPE
             RK3588_JOBS RK3588_ENABLE_XDP RK3588_ENABLE_USDT
EOF
	exit 0
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--sysroot) SYSROOT="$2"; shift 2 ;;
	--toolchain) TOOLCHAIN="$2"; shift 2 ;;
	--stages) STAGES="$2"; shift 2 ;;
	--buildtype) BUILDTYPE="$2"; shift 2 ;;
	--jobs) JOBS="$2"; shift 2 ;;
	--clean) CLEAN=1; shift ;;
	--fetch) FETCH=1; shift ;;
	--no-xdp) ENABLE_XDP=0; shift ;;
	--no-apps-rx) MP4_RX=0; shift ;;
	--usdt) ENABLE_USDT=1; shift ;;
	--patch-stdbool) PATCH_BOOL=1; shift ;;
	-h|--help) show_help ;;
	*) log_error "Unknown option: $1"; show_help ;;
	esac
done

# ---------- auto-detect sysroot ----------
if [[ -z "$SYSROOT" ]]; then
	for cand in \
		"$repo_dir/sysroot" \
		"/home/kkkqkx/mtl/sysroot" \
		"/home/kkkqkx/code/mtl/sysroot" \
		"/data/kkkqkx/mtl/sysroot"; do
		if [[ -d "$cand/usr/include" ]]; then SYSROOT="$cand"; break; fi
	done
fi
if [[ -z "$SYSROOT" || ! -d "$SYSROOT/usr/include" ]]; then
	log_error "sysroot not found. Pass --sysroot or set RK3588_SYSROOT."
	exit 1
fi
SYSROOT=$(readlink -f "$SYSROOT")

# ---------- auto-detect toolchain ----------
if [[ -z "$TOOLCHAIN" ]]; then
	for cand in \
		"/home/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu" \
		"/data/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu"; do
		if [[ -x "$cand/bin/aarch64-none-linux-gnu-gcc" ]]; then TOOLCHAIN="$cand"; break; fi
	done
fi
# also search
if [[ -z "$TOOLCHAIN" ]]; then
	TOOLCHAIN=$(find /home /data /opt -maxdepth 4 -name 'aarch64-none-linux-gnu-gcc' -type f 2>/dev/null | head -1 | xargs -r dirname | xargs -r dirname)
fi
if [[ -z "$TOOLCHAIN" || ! -x "$TOOLCHAIN/bin/aarch64-none-linux-gnu-gcc" ]]; then
	log_error "ARM GNU Toolchain not found. Pass --toolchain or set RK3588_TOOLCHAIN."
	exit 1
fi
TOOLCHAIN=$(readlink -f "$TOOLCHAIN")

# ---------- stage parsing ----------
run_stage() { case ",$STAGES," in *",$1,"*) return 0;; esac; [[ "$STAGES" == "all" ]]; }
# normalize
if [[ "$STAGES" == "all" ]]; then STAGES="dpdk,xdp,mtl,manager,apps"; fi

# ---------- wrappers ----------
BIN_DIR="$SYSROOT/../bin"
# keep bin next to sysroot parent (mirrors verified layout); fallback repo/bin
if [[ ! -d "$BIN_DIR" ]]; then BIN_DIR="$repo_dir/cross-bin"; fi
mkdir -p "$BIN_DIR"

GCC="$TOOLCHAIN/bin/aarch64-none-linux-gnu-gcc"
GXX="$TOOLCHAIN/bin/aarch64-none-linux-gnu-g++"
AR="aarch64-none-linux-gnu-ar"
STRIP="aarch64-none-linux-gnu-strip"

CC_WRAP="$BIN_DIR/aarch64-cc-wrap"
CXX_WRAP="$BIN_DIR/aarch64-cxx-wrap"
PKG_WRAP="$BIN_DIR/mtl-pkg-config"

write_wrappers() {
	cat > "$CC_WRAP" <<EOF
#!/bin/sh
SYSROOT=$SYSROOT
exec "$GCC" --sysroot="\$SYSROOT" -B"\$SYSROOT/usr/lib/aarch64-linux-gnu" -L"\$SYSROOT/usr/lib/aarch64-linux-gnu" -L"\$SYSROOT/lib/aarch64-linux-gnu" -isystem"\$SYSROOT/usr/include/aarch64-linux-gnu" "\$@" -Wno-error=unused-variable -Wno-error=unused-but-set-variable -Wl,-rpath-link,"\$SYSROOT/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"\$SYSROOT/lib/aarch64-linux-gnu" -lm
EOF
	cat > "$CXX_WRAP" <<EOF
#!/bin/sh
SYSROOT=$SYSROOT
exec "$TOOLCHAIN/bin/aarch64-none-linux-gnu-g++" --sysroot="\$SYSROOT" -B"\$SYSROOT/usr/lib/aarch64-linux-gnu" -L"\$SYSROOT/usr/lib/aarch64-linux-gnu" -L"\$SYSROOT/lib/aarch64-linux-gnu" -isystem"\$SYSROOT/usr/include/aarch64-linux-gnu" "\$@" -Wno-error=unused-variable -Wno-error=unused-but-set-variable -Wl,-rpath-link,"\$SYSROOT/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"\$SYSROOT/lib/aarch64-linux-gnu" -lm
EOF
	cat > "$PKG_WRAP" <<EOF
#!/bin/sh
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig
exec pkg-config "\$@"
EOF
	chmod +x "$CC_WRAP" "$CXX_WRAP" "$PKG_WRAP"
}
write_wrappers

# ---------- cross file ----------
CROSS_FILE="$SYSROOT/aarch64.ini"
cat > "$CROSS_FILE" <<EOF
[binaries]
c = '$CC_WRAP'
cpp = '$CXX_WRAP'
ar = '$AR'
strip = '$STRIP'
pkg-config = '$PKG_WRAP'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
platform = 'generic'
c_args = ['--sysroot=$SYSROOT', '-isystem$SYSROOT/usr/include/aarch64-linux-gnu']
c_link_args = ['--sysroot=$SYSROOT', '-B$SYSROOT/usr/lib/aarch64-linux-gnu', '-L$SYSROOT/usr/lib/aarch64-linux-gnu', '-L$SYSROOT/lib/aarch64-linux-gnu']
cpp_args = c_args
cpp_link_args = c_link_args
EOF

# ---------- env for pkg-config ----------
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig"
export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig:$SYSROOT/lib/aarch64-linux-gnu/pkgconfig"
export PATH="$TOOLCHAIN/bin:$PATH"

log_info "repo      : $repo_dir"
log_info "sysroot   : $SYSROOT"
log_info "toolchain : $TOOLCHAIN"
log_info "cross file: $CROSS_FILE"
log_info "stages    : $STAGES"

# ---------- helpers ----------
ensure_numaif_patch() {
	local f="$SYSROOT/usr/include/numaif.h"
	if [[ -f "$f" ]] && ! grep -q "linux/mempolicy.h" "$f"; then
		# prepend include guard-style include
		cp "$f" "$f.bak"
		{ echo "#include <linux/mempolicy.h>"; cat "$f.bak"; } > "$f"
		rm -f "$f.bak"
		log_info "patched numaif.h (MPOL_LOCAL)"
	fi
}

ensure_ffmpeg_symlinks() {
	local d="$SYSROOT/usr/lib/aarch64-linux-gnu"
	for name in avformat avcodec avutil; do
		if [[ -f "$d/lib${name}.so."* ]] && [[ ! -e "$d/lib${name}.so" ]]; then
			local target
			target=$(ls "$d/lib${name}.so."* 2>/dev/null | head -1)
			[[ -n "$target" ]] && ln -sf "$(basename "$target")" "$d/lib${name}.so"
		fi
	done
}

patch_stdbool() {
	python3 - "$repo_dir" <<'PY'
import os, glob
root = __import__('sys').argv[1]
for app in ('app/rk3588_live_tx', 'app/rk3588_playout_rx'):
    base = os.path.join(root, app)
    for dp,_,fs in os.walk(base):
        for f in fs:
            if not f.endswith('.c'): continue
            p = os.path.join(dp,f)
            s = open(p).read()
            if '#include <stdbool.h>' in s: continue
            # insert after first include or at top
            lines = s.splitlines(keepends=True)
            out = []
            inserted = False
            for ln in lines:
                out.append(ln)
                if not inserted and ln.strip().startswith('#include'):
                    out.append('#include <stdbool.h>\n')
                    inserted = True
            if not inserted:
                out.insert(0, '#include <stdbool.h>\n')
            open(p,'w').write(''.join(out))
print('stdbool.h patch done')
PY
}

# ---------- fetch sources ----------
fetch_sources() {
	local dpdk_ver xdp_ver
	dpdk_ver=$(grep '^DPDK_VER=' "$repo_dir/versions.env" | cut -d= -f2)
	xdp_ver=$(grep '^XDP_TOOLS_VER=' "$repo_dir/versions.env" | cut -d= -f2)
	if [[ ! -d "$repo_dir/dpdk-$dpdk_ver" ]]; then
		log_info "fetching DPDK $dpdk_ver"
		(cd "$repo_dir" && curl -LO "https://fast.dpdk.org/rel/dpdk-$dpdk_ver.tar.xz" && tar xf "dpdk-$dpdk_ver.tar.xz")
	fi
	if [[ "$ENABLE_XDP" == "1" && ! -d "$repo_dir/xdp-tools-$xdp_ver" ]]; then
		log_info "fetching xdp-tools $xdp_ver"
		(cd "$repo_dir" && curl -LO "https://github.com/xdp-project/xdp-tools/archive/refs/tags/v$xdp_ver.zip" && unzip -q "v$xdp_ver.zip")
	fi
}

# ---------- build functions ----------
build_dpdk() {
	run_stage dpdk || return 0
	local dpdk_ver
	dpdk_ver=$(grep '^DPDK_VER=' "$repo_dir/versions.env" | cut -d= -f2)
	local src="$repo_dir/dpdk-$dpdk_ver"
	if [[ ! -d "$src" ]]; then
		if [[ "$FETCH" == "1" ]]; then fetch_sources; else
			log_error "DPDK source missing: $src (use --fetch)"; exit 1; fi
	fi
	cd "$src"
	if [[ "$CLEAN" == "1" ]]; then rm -rf build-aarch64; fi
	# apply patches if not already
	if [[ ! -f build-aarch64/build.ninja ]]; then
		for p in "$repo_dir"/patches/dpdk/"$dpdk_ver"/*.patch; do
			[[ -e "$p" ]] || continue
			patch -p1 -N -i "$p" 2>/dev/null || true
		done
		meson setup build-aarch64 --cross-file "$CROSS_FILE" \
			-Dplatform=generic -Dcpu_instruction_set=armv8-a \
			-Denable_drivers=net/null,net/af_xdp,bus/pci,bus/vdev,common/iavf \
			-Dtests=false -Dexamples= --prefix=/usr
	fi
	ninja -C build-aarch64 -j"$JOBS"
	DESTDIR="$SYSROOT" ninja -C build-aarch64 install
	log_success "DPDK done"
}

build_xdp() {
	[[ "$ENABLE_XDP" == "1" ]] || { log_info "XDP disabled, skipping"; return 0; }
	run_stage xdp || return 0
	local xdp_ver
	xdp_ver=$(grep '^XDP_TOOLS_VER=' "$repo_dir/versions.env" | cut -d= -f2)
	local src="$repo_dir/xdp-tools-$xdp_ver"
	if [[ ! -d "$src" ]]; then
		if [[ "$FETCH" == "1" ]]; then fetch_sources; else
			log_error "xdp-tools source missing: $src (use --fetch)"; exit 1; fi
	fi
	cd "$src"
	if [[ "$CLEAN" == "1" ]]; then make clean >/dev/null 2>&1 || true; fi
	export PKG_CONFIG="$PKG_WRAP"
	export CC="$CC_WRAP"
	export CLANG="${CLANG:-clang}"
	export BPF_TARGET=bpf
	export PREFIX=/usr
	./configure
	make -C lib/libxdp libxdp.so libxdp.pc -j"$JOBS"
	# install into sysroot
	mkdir -p "$SYSROOT/usr/lib/aarch64-linux-gnu" "$SYSROOT/usr/include/xdp"
	cp -a lib/libxdp/libxdp.so* "$SYSROOT/usr/lib/aarch64-linux-gnu/" 2>/dev/null || true
	cp -a lib/libxdp/libxdp.pc "$SYSROOT/usr/lib/pkgconfig/" 2>/dev/null || true
	find "$src" -path '*/xdp/*.h' -name '*.h' -exec cp -t "$SYSROOT/usr/include/xdp" {} + 2>/dev/null || true
	log_success "xdp-tools done"
}

build_mtl() {
	run_stage mtl || return 0
	cd "$repo_dir"
	if [[ "$CLEAN" == "1" ]]; then rm -rf build-aarch64; fi
	ensure_numaif_patch
	ensure_ffmpeg_symlinks
	local usdt_opt="-Denable_usdt=false"
	[[ "$ENABLE_USDT" == "1" ]] && usdt_opt="-Denable_usdt=true"
	if [[ ! -f build-aarch64/build.ninja ]]; then
		meson setup build-aarch64 --cross-file "$CROSS_FILE" --prefix=/usr \
			"-Dbuildtype=$BUILDTYPE" $usdt_opt -Denable_tests=false
	fi
	ninja -C build-aarch64 -j"$JOBS"
	DESTDIR="$SYSROOT" ninja -C build-aarch64 install
	if grep -q MTL_HAS_XDP_BACKEND build-aarch64/config.h; then
		log_success "MTL done (XDP backend ENABLED)"
	else
		log_warning "MTL done (XDP backend OFF)"
	fi
}

build_manager() {
	run_stage manager || return 0
	cd "$repo_dir/manager"
	if [[ "$CLEAN" == "1" ]]; then rm -rf build-aarch64; fi
	if [[ ! -f build-aarch64/build.ninja ]]; then
		meson setup build-aarch64 --cross-file "$CROSS_FILE" --prefix=/usr
	fi
	ninja -C build-aarch64 -j"$JOBS"
	DESTDIR="$SYSROOT" ninja -C build-aarch64 install
	log_success "MtlManager done"
}

build_apps() {
	run_stage apps || return 0
	if [[ "$PATCH_BOOL" == "1" ]]; then patch_stdbool; fi
	# live_ingest
	cd "$repo_dir/app/rk3588_live_tx"
	if [[ "$CLEAN" == "1" ]]; then rm -rf build-aarch64; fi
	if [[ ! -f build-aarch64/build.ninja ]]; then
		meson setup build-aarch64 --cross-file "$CROSS_FILE"
	fi
	ninja -C build-aarch64 -j"$JOBS"
	# file_playout
	cd "$repo_dir/app/rk3588_playout_rx"
	if [[ "$CLEAN" == "1" ]]; then rm -rf build-aarch64; fi
	local opts="-Dbuild_tx=true"
	[[ "$MP4_RX" == "1" ]] && opts="$opts -Dbuild_rx=true" || opts="$opts -Dbuild_rx=false"
	if [[ ! -f build-aarch64/build.ninja ]]; then
		# shellcheck disable=SC2086
		meson setup build-aarch64 --cross-file "$CROSS_FILE" $opts
	fi
	ninja -C build-aarch64 -j"$JOBS"
	log_success "apps done"
}

assert_products() {
	log_info "asserting products..."
	test -f "$SYSROOT/usr/lib/aarch64-linux-gnu/libmtl.so" || { log_error "libmtl.so missing"; exit 1; }
	if run_stage apps; then
		test -x "$repo_dir/app/rk3588_live_tx/build-aarch64/live_ingest" || log_warning "live_ingest not built (dependency missing?)"
		test -x "$repo_dir/app/rk3588_playout_rx/build-aarch64/file_playout_tx" || log_warning "file_playout_tx not built"
		[[ "$MP4_RX" == "1" ]] && { test -x "$repo_dir/app/rk3588_playout_rx/build-aarch64/file_playout_rx" || log_warning "file_playout_rx not built"; }
	fi
	file "$SYSROOT/usr/lib/aarch64-linux-gnu/libmtl.so" 2>/dev/null | grep -q aarch64 && log_success "libmtl.so is aarch64"
}

# ---------- main ----------
ensure_ffmpeg_symlinks

if [[ "$PATCH_BOOL" == "1" ]]; then patch_stdbool; fi

run_stage dpdk && build_dpdk
run_stage xdp && build_xdp
run_stage mtl && build_mtl
run_stage manager && build_manager
run_stage apps && build_apps

assert_products
log_success "RK3588 cross build finished"
EOF
chmod +x script/build_rk3588_cross.sh