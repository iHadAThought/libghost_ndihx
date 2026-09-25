#!/usr/bin/env bash
# Install NDI SDK libndi + FFmpeg >= 7 shared libs + SDL2 build deps.
# Supports aarch64 and x86_64 Linux. Idempotent where practical.
set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
FFMPEG_TAG="${FFMPEG_TAG:-n7.1.1}"
NDI_INSTALLER_URL="${NDI_INSTALLER_URL:-https://downloads.ndi.tv/SDK/NDI_SDK_Linux/Install_NDI_SDK_v6_Linux.tar.gz}"
CACHE_DIR="${CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/ndi-hx-viewer}"
SKIP_FFMPEG="${SKIP_FFMPEG:-0}"
SKIP_NDI="${SKIP_NDI:-0}"

log() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

arch="$(uname -m)"
case "$arch" in
  aarch64|arm64)
    NDI_LIB_DIR="aarch64-rpi4-linux-gnueabi"
    ;;
  x86_64|amd64)
    NDI_LIB_DIR="x86_64-linux-gnu"
    ;;
  *)
    die "unsupported architecture '$arch' (need aarch64 or x86_64)"
    ;;
esac

if [[ "$(id -u)" -eq 0 ]]; then
  SUDO=()
else
  need_cmd sudo
  SUDO=(sudo)
fi

mkdir -p "$CACHE_DIR"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/ndi-hx-viewer.XXXXXX")"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

install_apt_packages() {
  if ! command -v apt-get >/dev/null 2>&1; then
    log "apt-get not found; skip package install (ensure build-essential, libsdl2-dev, nasm, pkg-config, curl, avahi are present)"
    return 0
  fi
  log "Installing apt packages…"
  "${SUDO[@]}" apt-get update -qq
  "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    curl \
    ca-certificates \
    nasm \
    yasm \
    libsdl2-dev \
    libavahi-client-dev \
    avahi-daemon \
    libsrt-openssl-dev || \
  "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    curl \
    ca-certificates \
    nasm \
    yasm \
    libsdl2-dev \
    libavahi-client-dev \
    avahi-daemon \
    libsrt-dev || true
}

ffmpeg_major_from_lib() {
  # Prefer libavcodec.so.N in PREFIX, else system.
  local so
  for so in "$PREFIX/lib/libavcodec.so".* /usr/lib/*/libavcodec.so.* /usr/lib/libavcodec.so.*; do
    [[ -e "$so" ]] || continue
    if [[ "$(basename "$so")" =~ libavcodec\.so\.([0-9]+) ]]; then
      echo "${BASH_REMATCH[1]}"
      return 0
    fi
  done
  return 1
}

need_ffmpeg7() {
  local major
  major="$(ffmpeg_major_from_lib || true)"
  if [[ -n "${major:-}" && "$major" -ge 61 ]]; then
    # FFmpeg 7.x → libavcodec.so.61
    return 1
  fi
  return 0
}

install_ffmpeg7() {
  if [[ "$SKIP_FFMPEG" == "1" ]]; then
    log "SKIP_FFMPEG=1 — not installing FFmpeg"
    return 0
  fi
  if ! need_ffmpeg7; then
    log "FFmpeg 7+ shared libs already present (libavcodec >= .61)"
    return 0
  fi

  need_cmd curl
  need_cmd tar
  need_cmd make

  local tarball="$CACHE_DIR/FFmpeg-${FFMPEG_TAG}.tar.gz"
  local url="https://github.com/FFmpeg/FFmpeg/archive/refs/tags/${FFMPEG_TAG}.tar.gz"
  if [[ ! -f "$tarball" ]]; then
    log "Downloading FFmpeg ${FFMPEG_TAG}…"
    curl -fL --retry 5 -o "$tarball.partial" "$url"
    mv "$tarball.partial" "$tarball"
  else
    log "Using cached $tarball"
  fi

  log "Building FFmpeg ${FFMPEG_TAG} (shared) into $PREFIX — this can take a while…"
  tar -xzf "$tarball" -C "$TMP"
  local src
  src="$(find "$TMP" -maxdepth 1 -type d -name 'FFmpeg-*' | head -1)"
  [[ -d "$src" ]] || die "FFmpeg source extract failed"
  pushd "$src" >/dev/null
  local srt_flag=()
  if pkg-config --exists srt 2>/dev/null || [[ -f /usr/include/srt/srt.h ]] || [[ -f /usr/local/include/srt/srt.h ]]; then
    srt_flag=(--enable-libsrt)
    log "Enabling libsrt for SRT demux"
  else
    log "libsrt headers not found — SRT demux disabled in this FFmpeg build"
  fi
  ./configure \
    --prefix="$PREFIX" \
    --enable-shared \
    --disable-static \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    --disable-programs \
    --disable-debug \
    --enable-pic \
    "${srt_flag[@]}"
  make -j"$(nproc)"
  "${SUDO[@]}" make install
  popd >/dev/null
  "${SUDO[@]}" ldconfig
  log "FFmpeg ${FFMPEG_TAG} installed under $PREFIX"
}

install_ndi() {
  if [[ "$SKIP_NDI" == "1" ]]; then
    log "SKIP_NDI=1 — not installing libndi"
    return 0
  fi

  if [[ -e "$PREFIX/lib/libndi.so" || -e "$PREFIX/lib/libndi.so.6" ]]; then
    log "libndi already in $PREFIX/lib — refreshing from SDK anyway"
  fi

  need_cmd curl
  need_cmd tar

  local tarball="$CACHE_DIR/Install_NDI_SDK_v6_Linux.tar.gz"
  if [[ ! -f "$tarball" ]]; then
    log "Downloading NDI SDK v6…"
    curl -fL --retry 5 -o "$tarball.partial" "$NDI_INSTALLER_URL"
    mv "$tarball.partial" "$tarball"
  else
    log "Using cached $tarball"
  fi

  log "Extracting NDI SDK…"
  tar -xzf "$tarball" -C "$TMP"
  pushd "$TMP" >/dev/null
  # Installer is a self-extracting shell script that prompts; feed yes.
  # `yes` exits 141 (SIGPIPE) when the installer closes stdin — that is success.
  local installer
  installer="$(find "$TMP" -maxdepth 1 -type f -name 'Install_NDI_SDK_v6_Linux*.sh' | head -1)"
  [[ -f "$installer" ]] || die "NDI installer script not found in archive"
  set +e
  yes y | PAGER=cat sh "$installer" >/dev/null
  local ndi_status=$?
  set -e
  if [[ "$ndi_status" -ne 0 && "$ndi_status" -ne 141 ]]; then
    die "NDI installer failed with status $ndi_status"
  fi
  local sdk="$TMP/NDI SDK for Linux"
  [[ -d "$sdk" ]] || die "NDI SDK directory missing after install script"
  local libdir="$sdk/lib/$NDI_LIB_DIR"
  [[ -d "$libdir" ]] || die "NDI lib dir missing: $libdir (arch=$arch)"

  log "Installing libndi from $libdir → $PREFIX/lib"
  "${SUDO[@]}" mkdir -p "$PREFIX/lib" "$PREFIX/include"
  "${SUDO[@]}" cp -a "$libdir"/libndi.so* "$PREFIX/lib/"
  if [[ -d "$sdk/include" ]]; then
    "${SUDO[@]}" cp -a "$sdk"/include/. "$PREFIX/include/"
  fi
  # Backward-compat soname some tools expect
  if [[ -e "$PREFIX/lib/libndi.so.6" && ! -e "$PREFIX/lib/libndi.so.5" ]]; then
    "${SUDO[@]}" ln -sfn libndi.so.6 "$PREFIX/lib/libndi.so.5"
  fi
  "${SUDO[@]}" ldconfig
  popd >/dev/null

  log "libndi installed:"
  ls -la "$PREFIX/lib"/libndi.so* || true
}

verify() {
  log "Verifying…"
  [[ -e "$PREFIX/lib/libndi.so" || -e "$PREFIX/lib/libndi.so.6" ]] || die "libndi missing after install"
  if need_ffmpeg7; then
    die "libavcodec.so.61+ (FFmpeg 7) still missing — HX decode will fail"
  fi
  if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists sdl2 || die "SDL2 pkg-config not found (install libsdl2-dev)"
  fi
  log "ldd libndi (first lines):"
  ldd "$PREFIX/lib/libndi.so" 2>/dev/null | head -30 || ldd "$PREFIX/lib"/libndi.so.* 2>/dev/null | head -30 || true
  log "OK — deps ready for ndi-hx-viewer (arch=$arch, NDI lib=$NDI_LIB_DIR)"
}

install_apt_packages
install_ffmpeg7
install_ndi
verify
