#!/bin/bash
#
# Install framestream as a boot service on the Steam Frame.
#
# Run this on the headset as root:
#     sudo bash install-service.sh
#
# The binary goes in /home/steamos/bin because /home survives SteamOS updates.
# The unit lives on the root partition, so a major SteamOS update can remove it;
# re-running this script puts it back.

set -euo pipefail

PORT="${PORT:-8090}"
BINDIR=/home/steamos/bin
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
UNIT=/etc/systemd/system/framestream.service

if [ "$(id -u)" -ne 0 ]; then
    echo "run me as root: sudo bash $0" >&2
    exit 1
fi

if ! command -v gcc >/dev/null; then
    echo "gcc not found" >&2
    exit 1
fi

echo "building framestream..."
gcc -O2 -g -Wall -o "$SRC_DIR/framestream" "$SRC_DIR/framestream.c" -ljpeg -lpthread -lm

mkdir -p "$BINDIR"
install -m 0755 "$SRC_DIR/framestream" "$BINDIR/framestream"
chown -R steamos:steamos "$BINDIR"

# framecap is handy alongside it for debugging; install it if it built too.
if [ -f "$SRC_DIR/framecap.c" ]; then
    gcc -O2 -Wall -o "$SRC_DIR/framecap" "$SRC_DIR/framecap.c" 2>/dev/null || true
    [ -f "$SRC_DIR/framecap" ] && install -m 0755 "$SRC_DIR/framecap" "$BINDIR/framecap"
fi

echo "writing $UNIT (port $PORT)..."
cat > "$UNIT" <<UNITEOF
[Unit]
Description=Steam Frame eye camera MJPEG stream for EyeTrackVR
Documentation=https://github.com/EyeTrackVR/EyeTrackVR
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$BINDIR/framestream --port $PORT
# Runs as root: pidfd_getfd() into XRService is gated by ptrace_may_access(),
# and SteamOS ships kernel.yama.ptrace_scope=1.
User=root
# The service is expected to start long before SteamVR does and to sit waiting;
# it only exits on a real failure, so always bring it back.
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNITEOF

systemctl daemon-reload
systemctl enable framestream.service
systemctl restart framestream.service

sleep 2
systemctl --no-pager --lines=15 status framestream.service || true

IP=$(ip -4 addr show scope global | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)

cat <<DONE

Installed and running.

  EyeTrackVR camera addresses:
      http://$IP:$PORT/0
      http://$IP:$PORT/1

  Preview page:  http://$IP:$PORT/
  Status JSON:   http://$IP:$PORT/status
  Logs:          journalctl -u framestream -f

Eye frames only exist while the headset is being worn, so both streams show a
blank frame until you put it on.
DONE
