# FrameEyeCameraFeed — Steam Frame eye camera capture

Captures the Steam Frame eye camera feeds and hosts them in a format accepted by EyeTrackVR.

Capture is achieved by borrowing the DMA-BUF file
descriptors that XRService fills.

See [Here](/EXPLAINED.MD) for an AI generated explanation of how this works to base your own projects off this.

# What to expect

This application provides the eye camera feeds from the Steam Frame in a format that EyeTrackVR (And Baballonia/other softrware) can accept.

EyeTrackVR seems to have some troubles with the gaze, however, it will provide lid position (Blink/Wink), pupil dilation, and can simulate eyebrow positions.

I've created a custom version of the EyeTrackVR VRCFT module that takes (And smoothes out) the Steam Link OSC gaze data and combines it with the EyeTrackVR data. I highly suggest using that alongside this instead of the regular EyeTrackVR VRCFT plugin.

[Recommended module for VRCFT: ETVRTrackingModule-SteamLink](https://github.com/Curtis-VL/ETVRTrackingModule-SteamLink)

https://github.com/user-attachments/assets/30928f5e-88ea-4b6f-920a-b49010649131


# Setup - Automatic (Easiest and fastest)

Check out the automatic installer here, this'll also guide you through setting it up for VRChat alongside EyeTrackVR!

[Automatical installer](https://github.com/Curtis-VL/FrameEyeCameraFeed-Installer/)

# Setup - Step-by-step

### Install FrameEyeCameraFeed on the Frame

You'll need to set the user password in the developer settings on the headset to connect to the headset via SSH or enter the password when prompted from 'Konsole' within the headset.

To install, run:

```bash
curl -fsSL https://github.com/Curtis-VL/FrameEyeCameraFeed/releases/latest/download/install.sh | sudo bash
```

This downloads the latest release, verifies its checksum, and installs it as a service that starts on boot.

Re-run the same command to update, or if a major SteamOS update removes the service.

Stream URLs for EyeTrackVR should now be available at:

http://frame:8090/1

http://frame:8090/0

The installer prints the exact addresses when it finishes, but the above should work regardless.

**Note:** It may be best to look at your router's DHCP settings to ensure the Frame's IP remains the same over time.


### Install EyeTrackVR

You can download the latest EyeTrackVR version from their GitHub repo:
[EyeTrackVR download](https://github.com/EyeTrackVR/EyeTrackVR/releases)

Once installed, enter the stream URLs above into the 'Address' fields and click Connect on the left. You should now see the eye camera feeds in EyeTrackVR. (If you don't put on the headset for a few seconds and check again)

With the headset on, click 'Start Calibration' and follow the instructions provided in VR.

Go to the 'Algo Settings' tab at the top and click 'Manual Eyelid Tuning'
Here you can set the point that is considered a full blink and when the eye is fully open to avoid half closed or half open eyes. Tune this as needed.

Optionally, go to the 'VRCFT Module Settings' tab at the top and turn on 'Emulate Eye Widen', 'Emulate Eye Squint', and 'Emulate eyebrows'

### Install the VRCFaceTracking plugin

Follow the instructions on the page for this custom EyeTrackVR VRCFaceTracking plugin, it is highly recommended you use this instead of the regular EyeTrackVR plugin as this one will merge in the Steam Link OSC gaze data to compensate for EyeTrackVR struggling to get accurate gaze data with the Frame.

[Recommended module for VRCFT: ETVRTrackingModule-SteamLink](https://github.com/Curtis-VL/ETVRTrackingModule-SteamLink)

### You're done!

**Tip:** You can use VRCX's auto-start feature to automatically start EyeTrackVR and VRCFaceTracking when you launch VRChat!

Enjoy showing everyone your eye balls!


# Check status

You can check the status of the application and preview the camera feeds at:

http://frame:8090

There's also a JSON status that you can hook into at:

http://frame:8090/status

<img width="837" height="498" alt="image" src="https://github.com/user-attachments/assets/2da6e4a1-0ae1-4047-8707-320909434236" />


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
