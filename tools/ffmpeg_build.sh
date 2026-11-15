#!/usr/bin/env bash
# The project's own audio-only FFmpeg for Windows x64, for the custom-music player (docs/CUSTOM-MUSIC-PLAN.md,
# "Getting FFmpeg"): cross-compiled on Ubuntu with mingw-w64 by .github/workflows/ffmpeg-release.yml.
#
#     tools/ffmpeg_build.sh <out dir>
#
# Writes to <out dir>: opensmash-ffmpeg-audio-<version>-win64.zip (bin/*.dll, COPYING.LGPLv2.1,
# configure-line.txt), ffmpeg-<version>.tar.xz (the source it was built from, for the LGPL) and
# SHA256SUMS. LGPL-2.1-or-later: no --enable-gpl, --enable-version3 or --enable-nonfree, no external libraries.
# Needs: gcc-mingw-w64-x86-64 (posix or win32 threads both fine: w32threads is used), make, curl, xz, zip.
# --disable-x86asm: no nasm needed; the plain C code is fast enough for decoding music.
set -euo pipefail

VERSION=7.1.5
SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f   # ffmpeg-7.1.5.tar.xz, ffmpeg.org/releases
OUT=$(mkdir -p "${1:?usage: ffmpeg_build.sh <out dir>}" && cd "$1" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

curl -fsSL -o "$OUT/ffmpeg-$VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
echo "$SHA256  $OUT/ffmpeg-$VERSION.tar.xz" | sha256sum -c -
tar -xf "$OUT/ffmpeg-$VERSION.tar.xz" -C "$WORK"

# AAC-LC and HE-AAC (SBR/PS) are both the native "aac" decoder; "mov" is .m4a (AAC or ALAC), "aac" is raw ADTS.
# mp3float is the MP3 decoder (the "mp3" name is the fixed-point one, not wanted). Ogg Vorbis and Ogg Opus share the "ogg" demuxer.
CONFIGURE=(
  --prefix=/install --cross-prefix=x86_64-w64-mingw32- --target-os=mingw32 --arch=x86_64
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
  --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc --disable-debug
  --disable-x86asm --disable-pthreads --enable-w32threads
  --enable-shared --disable-static --enable-swresample
  --extra-ldflags=-static-libgcc
  --enable-protocol=file
  --enable-demuxer=mp3,mov,flac,ogg,wav,aac
  --enable-parser=mpegaudio,aac,flac,opus,vorbis
  --enable-decoder=mp3float,aac,alac,flac,vorbis,opus,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le
)
printf '%s\n' "./configure ${CONFIGURE[*]}" > "$WORK/configure-line.txt"

cd "$WORK/ffmpeg-$VERSION"
./configure "${CONFIGURE[@]}"
make -j"$(nproc)"
make install DESTDIR="$WORK/dest"

PKG="opensmash-ffmpeg-audio-$VERSION-win64"
mkdir -p "$WORK/$PKG/bin"
cp "$WORK"/dest/install/bin/*.dll "$WORK/$PKG/bin/"
cp COPYING.LGPLv2.1 "$WORK/configure-line.txt" "$WORK/$PKG/"
x86_64-w64-mingw32-strip --strip-unneeded "$WORK/$PKG"/bin/*.dll
(cd "$WORK" && zip -qr "$OUT/$PKG.zip" "$PKG")
(cd "$OUT" && sha256sum "$PKG.zip" "ffmpeg-$VERSION.tar.xz" > SHA256SUMS && cat SHA256SUMS)
