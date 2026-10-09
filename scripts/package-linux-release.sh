#!/usr/bin/env bash
# Bundle MegaVideoProtect with the Qt, FFmpeg, and GStreamer libraries it
# loads, so the release tarball runs without a matching -dev install.
# Glibc itself stays on the system (Ubuntu 24.04 / glibc 2.39 or newer).
set -euo pipefail

bin="${1:?path to the MegaVideoProtect binary}"
archive="${2:?output tar.gz path}"

if [[ ! -x "$bin" ]]; then
  echo "Release binary not found: $bin" >&2
  exit 1
fi

stage="$(mktemp -d)"
root="$stage/MegaVideoProtect"
mkdir -p "$root/lib" "$root/plugins" "$root/gstreamer-1.0"

cp -L "$bin" "$root/MegaVideoProtect"
chmod +x "$root/MegaVideoProtect"

is_system_lib() {
  case "$1" in
    */libc.so.*|*/libm.so.*|*/libdl.so.*|*/libpthread.so.*|*/librt.so.*|*/libresolv.so.*|*/libutil.so.*|*/ld-linux-x86-64.so.*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

declare -A seen=()
queue=()

enqueue() {
  local file="$1"
  [[ -f "$file" ]] || return 0
  local real
  real="$(readlink -f "$file")"
  if [[ -n "${seen[$real]:-}" ]]; then
    return 0
  fi
  seen["$real"]=1
  queue+=("$real")
}

collect_deps() {
  local file="$1"
  local dep
  local listing
  listing="$(ldd "$file" 2>/dev/null || true)"
  while read -r dep; do
    [[ -n "$dep" && -f "$dep" ]] || continue
    if is_system_lib "$dep"; then
      continue
    fi
    enqueue "$dep"
  done < <(printf '%s\n' "$listing" | awk '/=>/ { print $3 }')
}

export QT_SELECT=qt5
plugindir=""
if command -v qmake >/dev/null 2>&1; then
  plugindir="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
fi
if [[ -z "$plugindir" || ! -d "$plugindir" ]]; then
  plugindir=/usr/lib/x86_64-linux-gnu/qt5/plugins
fi

for sub in platforms platforminputcontexts imageformats mediaservice audio iconengines xcbglintegrations; do
  if [[ -d "$plugindir/$sub" ]]; then
    mkdir -p "$root/plugins/$sub"
    cp -a "$plugindir/$sub/." "$root/plugins/$sub/"
  fi
done

gst_dir=/usr/lib/x86_64-linux-gnu/gstreamer-1.0
if [[ -d "$gst_dir" ]]; then
  cp -a "$gst_dir/." "$root/gstreamer-1.0/"
fi

enqueue "$root/MegaVideoProtect"
while IFS= read -r -d '' so; do
  enqueue "$so"
done < <(find "$root/plugins" "$root/gstreamer-1.0" -type f -name '*.so' -print0)

index=0
while [[ "$index" -lt "${#queue[@]}" ]]; do
  collect_deps "${queue[$index]}"
  index=$((index + 1))
done

app_real="$(readlink -f "$root/MegaVideoProtect")"
for file in "${queue[@]}"; do
  [[ "$file" == "$app_real" ]] && continue
  case "$file" in
    "$root"/*) continue ;;
  esac
  cp -L "$file" "$root/lib/$(basename "$file")"
done

cat > "$root/MegaVideoProtect.sh" << 'EOF'
#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms"
export GST_PLUGIN_PATH="$HERE/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH_1_0="$HERE/gstreamer-1.0"
exec "$HERE/MegaVideoProtect" "$@"
EOF
chmod +x "$root/MegaVideoProtect.sh"

cat > "$root/README.txt" << 'EOF'
Mega Video Protect for Linux x64.

Run:
  ./MegaVideoProtect.sh

This build is linked on Ubuntu 24.04. It needs glibc 2.39 or newer
(Ubuntu 24.04, or another current distro). Extract the archive and
start the shell script above; the libraries it needs are in lib/.
EOF

tar -C "$stage" -czf "$archive" MegaVideoProtect
echo "Packaged $archive ($(wc -c < "$archive") bytes)"
