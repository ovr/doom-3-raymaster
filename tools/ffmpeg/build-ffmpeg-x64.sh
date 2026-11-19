#!/usr/bin/env bash
# The minimal 64-bit LGPL FFmpeg (bink demuxer and decoders) as MSVC DLLs in external/ffmpeg-x64.
# Run it through build-ffmpeg-x64.cmd, or from MSYS2 bash inside the x64 VS 2022 prompt with
# MSYS2_PATH_TYPE=inherit.

set -euo pipefail

usage() {
	cat <<'USAGE'
build-ffmpeg-x64.sh [--prefix DIR] [--work DIR] [--asm|--no-asm] [--jobs N]
  --prefix  install dir, default <repo>/external/ffmpeg-x64 (include/, lib/, bin/)
  --work    download + build dir, default <repo>/build/ffmpeg
  --asm     nasm for the x86 SIMD code (pacman -S nasm); --no-asm (default) builds plain C
MSYS2 needs: pacman -S make diffutils
USAGE
}

FFMPEG_VERSION="7.1.5"
# release signed with the FFmpeg release key FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8
FFMPEG_SHA256="de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"

prefix="${repo_dir}/external/ffmpeg-x64"
work="${repo_dir}/build/ffmpeg"
use_asm=0
jobs="$(nproc 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
	case "$1" in
		--prefix) prefix="$2"; shift 2 ;;
		--work) work="$2"; shift 2 ;;
		--asm) use_asm=1; shift ;;
		--no-asm) use_asm=0; shift ;;
		--jobs) jobs="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
done

die() { echo "error: $*" >&2; exit 1; }

# ---- toolchain checks ------------------------------------------------------------------------

command -v cl >/dev/null 2>&1 || die "cl.exe not on PATH: run from an x64 VS 2022 developer environment with MSYS2_PATH_TYPE=inherit (or use build-ffmpeg-x64.cmd)"
# the cl banner may be localized ("for x64", "для x64"): only look at the target arch token
cl_banner="$( (cl 2>&1 || true) | head -n1)"
case "$cl_banner" in
	*" x64"*) ;;
	*) die "cl.exe is not the x64 compiler (need the x64 Native Tools environment): ${cl_banner}" ;;
esac
for tool in make cmp curl tar xz sha256sum; do
	command -v "$tool" >/dev/null 2>&1 || die "'$tool' missing in MSYS2 (pacman -S make diffutils)"
done

# MSYS2's coreutils link.exe shadows MSVC's link.exe: put the MSVC bin dir first
msvc_bin="$(dirname "$(command -v cl)")"
export PATH="${msvc_bin}:${PATH}"
link_banner="$( (link 2>&1 || true) | head -n1)"
case "$link_banner" in
	*Microsoft*) ;;
	*) die "link.exe on PATH is not the MSVC linker: ${link_banner}" ;;
esac

if [ "$use_asm" = 1 ]; then
	command -v nasm >/dev/null 2>&1 || die "--asm needs nasm (pacman -S nasm)"
	asm_flags=()
else
	asm_flags=(--disable-x86asm)
fi

# ---- source ----------------------------------------------------------------------------------

mkdir -p "$work"
tarball="${work}/ffmpeg-${FFMPEG_VERSION}.tar.xz"
if [ ! -f "$tarball" ]; then
	echo "downloading ${FFMPEG_URL}"
	curl -fL --retry 3 -o "${tarball}.part" "$FFMPEG_URL"
	mv "${tarball}.part" "$tarball"
fi
echo "${FFMPEG_SHA256}  ${tarball}" | sha256sum -c - || die "SHA-256 mismatch for ${tarball}"

src="${work}/ffmpeg-${FFMPEG_VERSION}"
rm -rf "$src"
tar -xJf "$tarball" -C "$work"

obj="${work}/build-x64"
rm -rf "$obj"
mkdir -p "$obj"

# ---- configure + build -----------------------------------------------------------------------

cd "$obj"
"${src}/configure" \
	--prefix="$prefix" \
	--toolchain=msvc \
	--arch=x86_64 \
	--target-os=win32 \
	--enable-shared \
	--disable-static \
	--disable-autodetect \
	--disable-everything \
	--disable-programs \
	--disable-doc \
	--disable-network \
	--disable-avdevice \
	--disable-avfilter \
	--disable-swscale \
	--disable-swresample \
	--disable-postproc \
	--enable-decoder=bink,binkaudio_rdft,binkaudio_dct \
	--enable-demuxer=bink \
	--enable-protocol=file \
	"${asm_flags[@]}"

grep -q 'FFMPEG_LICENSE "LGPL' config.h || die "configured build is not LGPL"

make -j"$jobs"
rm -rf "$prefix"
make install

echo
echo "FFmpeg ${FFMPEG_VERSION} installed into ${prefix}"
ls "${prefix}/bin" "${prefix}/lib"
