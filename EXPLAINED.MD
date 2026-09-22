## How it works

**Note:** This document is AI generated, take it all with a grain of salt to create your own projects!

Capture is achieved by borrowing the DMA-BUF file
descriptors that XRService fills.

```bash
gcc -O2 -Wall -o framecap framecap.c
```

Everything is discovered at runtime — no hardcoded PIDs, FD numbers, buffer
sizes or resolutions. Run it as root (`pidfd_getfd()` is blocked by
`kernel.yama.ptrace_scope=1`).

```bash
sudo ./framecap --list                        # show cameras and buffer queues
sudo ./framecap --eye --frames 30             # eye tracking cameras
sudo ./framecap --camera 5-0060 --frames 10   # one tracking camera by sensor
sudo ./framecap --camera all --snapshot       # dump every buffer as-is
```

Frames are written as 8-bit PGM.

## The cameras

Six sensors sit on `/dev/media0`, but only four ever stream, and **all four are
exterior tracking cameras**:

| Sensor | Node | Format | Real pitch |
|---|---|---|---|
| `og01a1b 4-0036` | `/dev/video0` | 1056×1024 NV12 | 1152 |
| `og01a1b 4-0060` | `/dev/video3` | 1056×1024 NV12 | 1152 |
| `og0ve10 5-003e` | `/dev/video9` | 640×480 GREY | 640 |
| `og0ve10 5-0060` | `/dev/video13` | 640×480 GREY | 640 |

The `arcimx616` pair (2464×2464 Y10) stay `runtime_status=suspended` even while
eye tracking is running, so they are not the eye cameras either.

## The eye cameras

The eye cameras are **not V4L2 devices** — nothing in the media graph carries
them, and the media graph is identical whether or not the headset is worn.

Their frames only surface in the udmabuf XRService shares with the
`eyetracking` helper, which hands them to the Hexagon DSP for gaze estimation
(`CGazeEstimatorCdsp` in `~/.local/share/Steam/logs/eyetracking.txt`). In that
buffer each frame is 8-bit greyscale, **512 bytes per row, 400 rows, one frame
per 256 KiB slot**, carrying both eyes side by side, with a small float header
(exposure/gain) just before it.

Those slot offsets are sub-allocations inside a shared heap and move between
runs, so `--eye` finds them instead of assuming them: it watches for 64 KiB
blocks that change, then scores the live regions for image-likeness (low
difference between vertically adjacent pixels) and tracks the slots that pass.

## The headset must be worn

Cameras only stream while SteamVR is out of standby, which is driven by the
`vcnl4000` proximity sensor:

```bash
cat /sys/bus/iio/devices/iio:device2/in_proximity_raw   # ~2.7 idle, >30 covered
```

Covering it (a thumb or a piece of tape on the sensor between the lenses) makes
SteamVR leave standby and resume both tracking and eye tracking. With the
headset idle, `--snapshot` still dumps whatever frames are left in the queues
from the last session.

## Notes for anyone extending this

- `qcom-camss` under-reports `bytesperline`. Derive the real pitch from plane 0
  `sizeimage` — the og01a1b nodes claim 1056 but the VFE writes 1152.
- Plane 0 is allocated with slack (the 640×480 nodes report 307200 but get a
  462848-byte buffer). Plane 1's size matches `sizeimage` exactly, so that is
  the reliable key when matching buffers to cameras.
- A CSIPHY carries two sensors on separate pad pairs, so walking the media graph
  upstream has to follow the *paired* sink pad (index = source index − 1) rather
  than the first enabled link, or every camera resolves to the same sensor.
- XRService allocates each queue right after opening that sensor's subdev, so
  the nearest preceding subdev descriptor is what attributes a run of buffers to
  a camera. Splitting on FD-number gaps does not work — the gaps inside a queue
  are sometimes larger than the gaps between queues.

---

# framestream — MJPEG for EyeTrackVR

Serves the eye cameras as MJPEG over HTTP so they can be pasted straight into
EyeTrackVR's camera address field.

```bash
gcc -O2 -Wall -o framestream framestream.c -ljpeg -lpthread -lm
sudo ./framestream --port 8090
```

EyeTrackVR treats the address as a serial port only when it starts with `COM`,
`/dev/cu` or `/dev/tty`; anything else goes straight to
`cv2.VideoCapture(addr, CAP_ANY)`, which handles MJPEG-over-HTTP via FFmpeg. So
these work as-is:

```
http://192.168.0.210:8090/0      one eye
http://192.168.0.210:8090/1      the other
```

| Endpoint | What it is |
|---|---|
| `/0` `/1` | MJPEG streams (`multipart/x-mixed-replace`), one eye each |
| `/left` `/right` | aliases for `/0` and `/1` |
| `/2` or `/full` | newest frame with no eye classification — use it to sanity-check framing |
| `/0.jpg` | single JPEG, handy for testing |
| `/status` | JSON: discovery state, ring slots, frame counts |
| `/` | preview page showing both streams |

Options: `--port`, `--quality`, `--fps`, `--scale`, `--crop X,Y,W,H`, `--swap`.
`--swap` exchanges `/0` and `/1`: nothing in the buffer says which eye is
which, so that assignment is arbitrary — check `/` in a browser and add
`--swap` if they are the wrong way round.

## How the frames are laid out

All of this was measured on the device, not inferred — several plausible-looking
assumptions turned out to be wrong.

- Eight frames sit in one ring in the shared buffer, 512×400 8-bit grey each.
- The spacing is **262208 bytes**, not the 256 KiB it appears to be. Those extra
  64 bytes per slot are what made the picture appear to scan sideways when read
  on a 256 KiB grid: each slot came out rotated 64 more pixels than the last,
  wrapping exactly one row across the eight. The pitch is still measured at
  runtime (by asking how far one slot must rotate to line up with the previous)
  rather than hardcoded.
- **Slots 0–3 hold one eye and 4–7 the other.** Cross-correlating the slots
  showed a 10× gap in residual between the two groups. So the eye is fixed by
  slot and is decided once at lock time. Deciding it per frame from picture
  content — which an earlier version did — can always flip, and that is exactly
  what made one stream flicker between eyes.
- The two cameras are mounted rotated relative to each other, so eye 1 arrives
  upside down and is flipped vertically by default; eye 0 needs no correction.
  `--flip0` / `--flip1` (`none`, `v`, `h`, `vh`) set this per physical camera,
  so a correction follows the camera rather than the stream number and stays
  right under `--swap`.

## Not locking onto the wrong memory

The buffer is a general heap and gets recycled once eye tracking stops, so
"looks like an image" is a surprisingly weak test. Measured on real frames
versus the memory this actually locked onto by mistake:

| | real eye frames | recycled heap |
|---|---|---|
| lit fraction | 0.27 – 0.34 | 0.09 – 0.15 |
| local roughness | 6.3 – 7.2 | 3.5 – 3.8 |

The **lit fraction** is what separates them. Roughness does not — the junk is a
mostly-black frame with a thin band of noise, so it is *smoother* on average
than a real eye, which is precisely why an average row-to-row difference waved
it through and served noise for several minutes.

Three things now have to hold before a lock is accepted: the proximity sensor
must say the headset is being worn (the eye cameras only run then), at least
20% of the frame must be lit, and the candidate must keep refreshing for a
full second. Slots that then fail to refresh are dropped, which also removes
the occasional spurious slot past the end of the ring.

Port 8090 is the default because Steam's `steamwebhelper` already listens on
8080.

## Starting at boot

```bash
sudo bash install-service.sh
```

That builds `framestream`, installs it to `/home/steamos/bin` (which survives
SteamOS updates), writes `/etc/systemd/system/framestream.service`, enables it
and starts it. A major SteamOS update can replace the root partition and remove
the unit — re-running the script restores it.

```bash
systemctl status framestream
journalctl -u framestream -f
```

## Coming up before SteamVR does

At boot none of what this depends on exists yet, so none of it is treated as an
error:

- The HTTP server binds and serves **before** discovery starts, handing out a
  blank placeholder frame. A client that connects early is never refused, and
  switches to live frames without reconnecting.
- Missing SteamVR, missing XRService, a missing shared buffer and an idle
  headset are all states it waits in, retrying forever. `/status` says which.
- If XRService restarts, it notices, releases the old buffers and re-attaches.
- Eye frames only exist while the headset is worn. It locks on when they start
  flowing, and keeps serving the last good frame when they stop so clients stay
  connected, then re-scans in case the slots move when it is put back on.

Two guards stop it locking onto the wrong memory. The shared arena is a general
heap, so something in it occasionally looks image-like: candidates must have
real dynamic range (not a flat or near-black region), **and** must keep
refreshing at a sustained rate for a full second before being accepted. A live
eye camera runs at roughly 60 Hz and passes easily; incidental heap churn does
not. Without the rate check the daemon would lock onto noise while the headset
sat on a desk — it did exactly that during development.
