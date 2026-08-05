#!/usr/bin/env bash
#
# Build PPSSPP's pinned FFmpeg for Switch and install it over the portlibs
# prefix.
#
# Why this exists
# ---------------
# Every platform in PPSSPP's libretro Makefile points at the ffmpeg submodule
# it pins:
#
#     FFMPEGINCFLAGS += -I$(FFMPEGDIR)/linux/aarch64/include
#
# except libnx, which points at the system one:
#
#     FFMPEGINCFLAGS += -I$(PORTLIBS)/include
#
# That was fine when devkitPro shipped switch-ffmpeg 4.x. It now ships 7.1,
# and PPSSPP is written against the 3.x/4.x API, so the build dies in PSP
# audio decode on avcodec_decode_audio4, av_free_packet, swr_alloc_set_opts
# and friends. Nothing is broken in either project; the toolchain moved and
# the fork did not.
#
# ppsspp-ffmpeg carries prebuilt archives for android, ios, mac and eight
# Linux arches, and no Switch build script at all, so this is that script.
#
# Installing over the portlibs prefix, rather than into the source tree, is
# deliberate. The core is built with STATIC_LINKING, so it only consumes
# ffmpeg *headers*; the actual libraries come from RetroArch's own link line
# in Makefile.libnx:
#
#     LIBS := -lswresample -lavformat -lavcodec -lavutil -lswscale ...
#
# Compiling the core against 3.0.2 headers while RetroArch linked 7.1 would
# produce undefined references to functions that no longer exist, at the very
# last step. Replacing the prefix keeps both halves on one version.
#
# This is safe for RetroArch: Makefile.libnx never defines HAVE_FFMPEG, so no
# RetroArch source compiles against these headers. The libraries are on the
# link line purely to satisfy cores that reference them.
#
# The container is thrown away per job, so this affects nothing else.
set -euo pipefail

FFMPEG_SRC="${1:?usage: build-ffmpeg-switch.sh <path to ffmpeg source tree>}"

: "${DEVKITPRO:=/opt/devkitpro}"
PORTLIBS_PREFIX="${PORTLIBS_PREFIX:-$DEVKITPRO/portlibs/switch}"

cd "$FFMPEG_SRC"

echo "=== ffmpeg source ==="
if [ -f RELEASE ]; then
  echo "  version $(cat RELEASE)"
else
  echo "  version unknown (no RELEASE file)"
fi

# Only what PPSSPP actually decodes. Taken from the project's own
# shared_options.sh and linux_arm64.sh, unioned: PSP media is ATRAC3/ATRAC3+
# audio and H.264/MPEG-4 video, plus the PCM and MJPEG paths the camera and
# save-data code touch. Building everything else would be slower and would
# expose far more 2016-era code to a 2026 compiler for no benefit.
DECODERS="
  --enable-decoder=h264
  --enable-decoder=mpeg4
  --enable-decoder=mpeg2video
  --enable-decoder=mjpeg
  --enable-decoder=mjpegb
  --enable-decoder=aac
  --enable-decoder=aac_latm
  --enable-decoder=atrac3
  --enable-decoder=atrac3p
  --enable-decoder=mp3
  --enable-decoder=pcm_s16le
  --enable-decoder=pcm_s8"

DEMUXERS="
  --enable-demuxer=h264
  --enable-demuxer=m4v
  --enable-demuxer=mpegvideo
  --enable-demuxer=mpegps
  --enable-demuxer=mp3
  --enable-demuxer=avi
  --enable-demuxer=aac
  --enable-demuxer=pmp
  --enable-demuxer=oma
  --enable-demuxer=pcm_s16le
  --enable-demuxer=pcm_s8
  --enable-demuxer=wav"

PARSERS="
  --enable-parser=h264
  --enable-parser=mpeg4video
  --enable-parser=mpegaudio
  --enable-parser=mpegvideo
  --enable-parser=aac
  --enable-parser=aac_latm"

ENCODERS="
  --enable-encoder=huffyuv
  --enable-encoder=ffv1
  --enable-encoder=mjpeg
  --enable-encoder=pcm_s16le
  --enable-muxer=avi"

# Cross-compile settings lifted from devkitPro's own switch-ffmpeg PKGBUILD,
# with one substitution. That recipe passes --target-os=horizon, which is not
# an upstream ffmpeg target: it comes from a devkitPro patch whose 11,899
# lines are almost entirely nvtegra hardware decode. The horizon case itself
# does only three things -- enable section_data_rel_ro, add -lnx, and disable
# the sysctl probes -- so --target-os=none with -lnx passed explicitly is the
# same build for our purposes. We enable no hwaccel, so none of the rest of
# that patch is relevant.
#
# sysctl is left to autodetection: the probe is a link test, and newlib has
# no sysctl to find.
CFLAGS_NX="-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57"
CFLAGS_NX="$CFLAGS_NX -mtp=soft -fPIC -ftls-model=local-exec"
CFLAGS_NX="$CFLAGS_NX -I$PORTLIBS_PREFIX/include -I$DEVKITPRO/libnx/include"

echo "=== configure ==="
# shellcheck disable=SC2086
./configure \
  --prefix="$PORTLIBS_PREFIX" \
  --enable-cross-compile \
  --cross-prefix=aarch64-none-elf- \
  --arch=aarch64 \
  --cpu=cortex-a57 \
  --target-os=none \
  --enable-pic \
  --disable-shared \
  --enable-static \
  --extra-cflags="$CFLAGS_NX" \
  --extra-ldflags="-fPIE -L$PORTLIBS_PREFIX/lib -L$DEVKITPRO/libnx/lib" \
  --extra-libs="-lnx" \
  --disable-runtime-cpudetect \
  --disable-programs \
  --disable-doc \
  --disable-debug \
  --disable-network \
  --disable-avdevice \
  --disable-avfilter \
  --disable-postproc \
  --disable-iconv \
  --disable-lzma \
  --disable-bzlib \
  --enable-zlib \
  --disable-everything \
  $DECODERS \
  $DEMUXERS \
  $PARSERS \
  $ENCODERS \
  --enable-protocol=file

echo "=== build ==="
make -j"$(getconf _NPROCESSORS_ONLN)"

# Drop the 7.1 headers before installing 3.x over them. install only
# overwrites the files it ships, and a stale libavcodec header left beside a
# fresh one is the kind of mismatch that produces an error pointing at
# entirely the wrong thing.
echo "=== clearing the previous ffmpeg from $PORTLIBS_PREFIX ==="
rm -rf "$PORTLIBS_PREFIX"/include/libav* \
       "$PORTLIBS_PREFIX"/include/libsw* \
       "$PORTLIBS_PREFIX"/include/libpostproc \
       "$PORTLIBS_PREFIX"/lib/libav*.a \
       "$PORTLIBS_PREFIX"/lib/libsw*.a \
       "$PORTLIBS_PREFIX"/lib/libpostproc.a \
       "$PORTLIBS_PREFIX"/lib/pkgconfig/libav*.pc \
       "$PORTLIBS_PREFIX"/lib/pkgconfig/libsw*.pc

echo "=== install ==="
make install

echo "=== what is now in the prefix ==="
find "$PORTLIBS_PREFIX/lib" -maxdepth 1 \( -name 'libav*.a' -o -name 'libsw*.a' \) \
  -printf '  %10s  %f\n' 2>/dev/null | sort -k2

# Prove the swap took, rather than trusting that install did what it said.
# LIBAVCODEC_VERSION_MAJOR 57 is the 3.x series; 61 would mean 7.1 is still
# sitting there and the core is about to be compiled against the wrong API.
VER_H="$PORTLIBS_PREFIX/include/libavcodec/version.h"
MAJOR="$(sed -n 's/^#define LIBAVCODEC_VERSION_MAJOR[[:space:]]*//p' "$VER_H" 2>/dev/null | head -1)"
echo "=== LIBAVCODEC_VERSION_MAJOR is ${MAJOR:-unknown} ==="
if [ -z "$MAJOR" ]; then
  echo "could not read $VER_H, so the install cannot be confirmed"
  exit 1
fi
if [ "$MAJOR" -ge 59 ]; then
  echo "still a modern ffmpeg in the prefix; the install did not replace it"
  exit 1
fi

# The whole point of the exercise: this symbol is what PPSSPP failed on.
if ! grep -q 'avcodec_decode_audio4' "$PORTLIBS_PREFIX/include/libavcodec/avcodec.h"; then
  echo "avcodec_decode_audio4 is absent from the installed headers,"
  echo "so PPSSPP will fail the same way it did before."
  exit 1
fi
echo "ok: avcodec_decode_audio4 is declared, PPSSPP has the API it expects"
