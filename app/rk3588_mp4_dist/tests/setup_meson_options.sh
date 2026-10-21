#!/bin/bash
# Ensure meson_options.txt exists (required for -Dbuild_tx / -Dbuild_rx).
set -euo pipefail
CD="$(cd "$(dirname "$0")/.." && pwd)"
OPTS="$CD/meson_options.txt"
if [ -f "$OPTS" ]; then
  exit 0
fi
cat > "$OPTS" << 'EOF'
option('build_tx', type: 'boolean', value: true,
  description: 'Build mp4_dist_tx (FFmpeg demux + MTL TX)')
option('build_rx', type: 'boolean', value: true,
  description: 'Build mp4_dist_rx (MTL RX + MPP decode + DRM + ALSA)')
EOF
echo "Created $OPTS (was missing — sync this file from git in future)"
