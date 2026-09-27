# FrameEyeCameraFeed — Steam Frame eye camera capture

**Disclaimer:** This is heavily **AI generated**, use at your own risk. This project was made a proof-of-concept, something for other developers to use as a reference.

Captures the Steam Frame eye camera feeds and hosts them in a format accepted by EyeTrackVR.

Capture is achieved by borrowing the DMA-BUF file
descriptors that XRService fills.

See [Here](/EXPLAINED.MD) for an AI generated explanation of how this works to base your own projects off this.

# How to Use

On the Frame, open a terminal and run:

```bash
curl -fsSL https://github.com/Curtis-VL/FrameEyeCameraFeed/releases/latest/download/install.sh | sudo bash
```

This downloads the latest release, verifies its checksum, and installs it as a service that starts on boot.

Stream URLs for EyeTrackVR should now be available at:

http://frame:8090/1

http://frame:8090/0

The installer prints the exact addresses when it finishes, but the above should work regardless.

Options go after `sudo`:

| | |
|---|---|
| Different port | `... \| sudo PORT=8091 bash` |
| Extra options, e.g. swap eyes | `... \| sudo ARGS="--swap" bash` |
| Specific release | `... \| sudo VERSION=v1.0.0 bash` |
| Uninstall | `... \| sudo bash -s -- --uninstall` |

Re-run the same command to update, or if a major SteamOS update removes the service.

**You do this at your own risk.**
I'd advise only using this if you're also familiar enough with Linux to fix any issues that could come up.

# Troubleshooting

Eye camera streams are blank?
Put on the headset. The eye tracking service stops when the headset isn't on, thus no buffers to find for the stream.

Eye camera streams appear wrong?
Take off the headset for a few seconds, put it back on.