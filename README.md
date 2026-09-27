# FrameEyeCameraFeed — Steam Frame eye camera capture

**Disclaimer:** This is heavily **AI generated**, use at your own risk.

Captures the Steam Frame eye camera feeds and hosts them in a format accepted by EyeTrackVR.

Capture is achieved by borrowing the DMA-BUF file
descriptors that XRService fills.

See [Here](/EXPLAINED.MD) for an AI generated explanation of how this works to base your own projects off this.

# What to expect

This application provides the eye camera feeds from the Steam Frame in a format that EyeTrackVR can accept.

EyeTrackVR seems to have some troubles with the gaze, however, it will provide lid position (Blink/Wink) and also pupil dilation.

In my own testing, I used Steam Link's 'Enable OSC' and 'Share face tracking data to other apps on this PC via OSC' options for gaze, whilst using ETVR for lid position and pupil dilation.

The gaze tracking from Steam Link is jittery locally, but this jittery isn't really visible to others over the network.

https://github.com/user-attachments/assets/133d68aa-2698-489c-870e-a1b597f286f2

(There's no pupil dilation on my avatar, but you get the gist)


# How to Use

You'll need to set the user password in the developer settings on the headset to either connect to the headset via SSH, or to enter the admin password whem prompted to run the following command.

To install, run:

```bash
curl -fsSL https://github.com/Curtis-VL/FrameEyeCameraFeed/releases/latest/download/install.sh | sudo bash
```

This downloads the latest release, verifies its checksum, and installs it as a service that starts on boot.

Re-run the same command to update, or if a major SteamOS update removes the service.

Stream URLs for EyeTrackVR should now be available at:

http://frame-local-ip:8090/1

http://frame-local-ip:8090/0

The installer prints the exact addresses when it finishes, but the above should work regardless.

**Note:** It may be best to look at your router's DHCP settings to ensure the Frame's IP remains the same over time.

# Check status

You can check the status of the application and preview the camera feeds at:

http://frame-local-ip:8090

There's also a JSON status that you can hook into at:

http://frame-local-ip:8090/status

# Uninstall

```bash
curl -fsSL https://github.com/Curtis-VL/FrameEyeCameraFeed/releases/latest/download/install.sh | sudo bash -s -- --uninstall
```
# Options

Options go after `sudo`:

| | |
|---|---|
| Different port | `... \| sudo PORT=8091 bash` |
| Extra options, e.g. swap eyes | `... \| sudo ARGS="--swap" bash` |
| Specific release | `... \| sudo VERSION=v1.0.0 bash` |
| Uninstall | `... \| sudo bash -s -- --uninstall` |

# Troubleshooting

Eye camera streams are blank?
Put on the headset. The eye tracking service stops when the headset isn't on, thus no buffers to find for the stream.

Only /0 shows anything, and it keeps pausing?
That's what happens when the proximity sensor is just covered instead of the headset being worn: eye tracking then only runs one eye, in bursts. Put the headset on and the second eye gets picked up within a few seconds.

Eye camera streams appear wrong?
Take off the headset for a few seconds, put it back on.

# Disclaimer

**You use this at your own risk.**

I'd advise only using this if you're also familiar enough with Linux to fix any issues that could come up.

There's an awful lot of AI code in here, which I wouldn't normally be comfortable with posting publicly.

However, given that this is such a small project and likely to be replaced by Babble's own solution in the near future... It'll do for now!
