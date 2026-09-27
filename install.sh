#!/bin/bash
#
# Install framestream on the Steam Frame as a boot service.
#
# On the headset:
#
#     curl -fsSL https://github.com/Curtis-VL/FrameEyeCameraFeed/releases/latest/download/install.sh | sudo bash
#
# That downloads the prebuilt binaries from the latest GitHub release, checks
# them against SHA256SUMS, installs them and starts the service.  Nothing is
# compiled on the headset.
#
# Settings are environment variables, placed after sudo so they reach the script:
#
#     ... | sudo PORT=8091 bash                   different HTTP port
#     ... | sudo ARGS="--swap" bash               extra framestream options
#     ... | sudo VERSION=v1.0.0 bash              a specific release
#
# Other modes:
#
#     ... | sudo bash -s -- --uninstall           remove it again
#     sudo bash install.sh                        from a download or checkout:
#                                                 uses framestream-linux-arm64
#                                                 next to this script if present
#     sudo bash install.sh --from-source          build with gcc instead
#
# The binaries go in /home/steamos/bin because /home survives SteamOS updates.
# The unit lives on the root partition, so a major SteamOS update can remove
# it; running this again puts it back.

# Everything is inside one block so that when this is piped from curl, bash has
# to read all of it before running any - a cut-off download does nothing.
{

set -euo pipefail

REPO=Curtis-VL/FrameEyeCameraFeed
PORT="${PORT:-8090}"
ARGS="${ARGS:-}"
VERSION="${VERSION:-latest}"
UNIT=/etc/systemd/system/framestream.service

if [ -z "${BINDIR:-}" ]; then
    if [ -d /home/steamos ]; then
        BINDIR=/home/steamos/bin
    else
        BINDIR=/var/lib/framestream
    fi
fi

# Only look for local files when run as a file, not when piped from curl.
SELF="${BASH_SOURCE[0]:-}"
SRC_DIR=""
if [ -n "$SELF" ] && [ -f "$SELF" ]; then
    SRC_DIR="$(cd "$(dirname "$SELF")" && pwd)"
fi

MODE=install
for a in "$@"; do
    case "$a" in
        --uninstall)   MODE=uninstall ;;
        --from-source) MODE=source ;;
        -h|--help)     sed -n '2,30p' "${SELF:-/dev/null}" 2>/dev/null || true; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 1 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "run me as root (sudo)" >&2
    exit 1
fi

# ------------------------------------------------------------- uninstall

if [ "$MODE" = uninstall ]; then
    systemctl disable --now framestream.service 2>/dev/null || true
    rm -f "$UNIT"
    systemctl daemon-reload
    rm -f "$BINDIR/framestream" "$BINDIR/framecap"
    echo "framestream removed."
    exit 0
fi

# --------------------------------------------------------- get binaries

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fetch() {
    if command -v curl >/dev/null; then
        curl -fsSL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null; then
        wget -q -O "$2" "$1"
    else
        echo "need curl or wget" >&2
        return 1
    fi
}

if [ "$MODE" = source ]; then

    if [ -z "$SRC_DIR" ] || [ ! -f "$SRC_DIR/framestream.c" ]; then
        echo "--from-source needs framestream.c next to this script" >&2
        exit 1
    fi

    command -v gcc >/dev/null || { echo "gcc not found" >&2; exit 1; }

    echo "building from source..."
    gcc -O2 -Wall -o "$TMP/framestream" "$SRC_DIR/framestream.c" -ljpeg -lpthread -lm
    gcc -O2 -Wall -o "$TMP/framecap" "$SRC_DIR/framecap.c" || true

else

    case "$(uname -m)" in
        aarch64|arm64) ;;
        *)
            echo "prebuilt binaries are for arm64 (the Steam Frame), this is $(uname -m)." >&2
            echo "use --from-source from a checkout to build for this machine." >&2
            exit 1
            ;;
    esac

    if [ -n "$SRC_DIR" ] && [ -f "$SRC_DIR/framestream-linux-arm64" ]; then

        echo "using binaries from $SRC_DIR"
        cp "$SRC_DIR/framestream-linux-arm64" "$TMP/"
        [ -f "$SRC_DIR/framecap-linux-arm64" ] && cp "$SRC_DIR/framecap-linux-arm64" "$TMP/"
        [ -f "$SRC_DIR/SHA256SUMS" ] && cp "$SRC_DIR/SHA256SUMS" "$TMP/"

    else

        if [ "$VERSION" = latest ]; then
            BASE="https://github.com/$REPO/releases/latest/download"
        else
            BASE="https://github.com/$REPO/releases/download/$VERSION"
        fi

        echo "downloading $VERSION release from github.com/$REPO..."
        fetch "$BASE/framestream-linux-arm64" "$TMP/framestream-linux-arm64"
        fetch "$BASE/SHA256SUMS"              "$TMP/SHA256SUMS"
        fetch "$BASE/framecap-linux-arm64"    "$TMP/framecap-linux-arm64" ||
            rm -f "$TMP/framecap-linux-arm64"
    fi

    if [ -f "$TMP/SHA256SUMS" ]; then
        (cd "$TMP" && sha256sum -c --ignore-missing --quiet SHA256SUMS) || {
            echo "checksum mismatch - refusing to install" >&2
            exit 1
        }
    fi

    mv "$TMP/framestream-linux-arm64" "$TMP/framestream"
    [ -f "$TMP/framecap-linux-arm64" ] && mv "$TMP/framecap-linux-arm64" "$TMP/framecap"
fi

# ---------------------------------------------------------------- install

mkdir -p "$BINDIR"
install -m 0755 "$TMP/framestream" "$BINDIR/framestream"

# framecap is handy alongside it for debugging
[ -f "$TMP/framecap" ] && install -m 0755 "$TMP/framecap" "$BINDIR/framecap"

if [ "$BINDIR" = /home/steamos/bin ] && id steamos >/dev/null 2>&1; then
    chown -R steamos:steamos "$BINDIR"
fi

echo "writing $UNIT (port $PORT)..."
mkdir -p "$(dirname "$UNIT")"
cat > "$UNIT" <<UNITEOF
[Unit]
Description=Steam Frame eye camera MJPEG stream for EyeTrackVR
Documentation=https://github.com/$REPO
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$BINDIR/framestream --port $PORT $ARGS
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

IP=$(ip -4 addr show scope global 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)
IP="${IP:-<headset-ip>}"

cat <<DONE

Installed and running.

  EyeTrackVR camera addresses:
      http://$IP:$PORT/0
      http://$IP:$PORT/1

  Preview page:  http://$IP:$PORT/
  Status JSON:   http://$IP:$PORT/status
  Logs:          journalctl -u framestream -f
  Uninstall:     curl -fsSL https://github.com/$REPO/releases/latest/download/install.sh | sudo bash -s -- --uninstall

Eye frames only exist while the headset is being worn, so both streams show a
blank frame until you put it on.
DONE

exit 0
}
