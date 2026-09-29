#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
vendor="$root/vendor/ffmpeg"
revision=3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587
if [ ! -d "$vendor/.git" ]; then
    if [ -e "$vendor" ]; then
        echo "Refusing to overwrite $vendor" >&2
        exit 1
    fi
    mkdir -p "$vendor"
    git -C "$vendor" init -q
    git -C "$vendor" remote add origin https://github.com/FFmpeg/FFmpeg.git
    git -C "$vendor" fetch --depth=1 origin "$revision"
    git -C "$vendor" checkout -q --detach FETCH_HEAD
fi
if [ "$(git -C "$vendor" rev-parse HEAD)" != "$revision" ] ||
   [ -n "$(git -C "$vendor" status --porcelain)" ]; then
    echo "FFmpeg must be clean at $revision; no files were reset." >&2
    exit 1
fi
printf '%s\n' "$vendor"
