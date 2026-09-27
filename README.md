# FrameEyeCameraFeed — Steam Frame eye camera capture

**Disclaimer:** This is literally entirely **AI generated**, it was made purely as an experiment and **proof-of-concept**.
Use at your own risk.

Captures the Steam Frame eye camera feeds and hosts them in a format accepted by EyeTrackVR.

Capture is achieved by borrowing the DMA-BUF file
descriptors that XRService fills.

See [Here](/EXPLAINED.MD) for an AI generated explanation of how this works to base your own projects off this.

# How to Use

On the Frame, open a terminal and run:

```bash
curl -fsSL https://github.com/Curtis-VL/FrameEyeCameraFeed/releases/latest/download/install.sh | sudo bash
```

This downloads the prebuilt binary from the latest release, verifies its checksum, and installs it as a service that starts at boot. Nothing is compiled on the headset.

Stream URLs for EyeTrackVR should now be available at:

http://frame-local-IP:8090/1

http://frame-local-IP:8090/0

The installer prints the exact addresses when it finishes.

Options go after `sudo`:

| | |
|---|---|
| Different port | `... \| sudo PORT=8091 bash` |
| Extra options, e.g. swap eyes | `... \| sudo ARGS="--swap" bash` |
| Specific release | `... \| sudo VERSION=v1.0.0 bash` |
| Uninstall | `... \| sudo bash -s -- --uninstall` |

Re-run the same command to update, or after a major SteamOS update removes the service.

No internet on the headset? Download `install.sh`, `framestream-linux-arm64` and `SHA256SUMS` from the [releases page](https://github.com/Curtis-VL/FrameEyeCameraFeed/releases), copy them into one folder on the Frame and run `sudo bash install.sh` there. To build it yourself instead, run `sudo bash install.sh --from-source` in a checkout of this repo (needs gcc and libjpeg).

**You do this at your own risk.**
This repo is here only as a proof of concept, I'd highly advise against using this if you're not familiar with what you're doing or how to fix any problems that may arise!

# Troubleshooting

Eye camera streams are blank?
Put on the headset. The eye tracking service stops when the headset isn't on, thus no buffers to find for the stream.

Eye camera streams appear wrong?
Take off the headset for a few seconds, put it back on.