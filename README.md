# FrameEyeCameraFeed — Steam Frame eye camera capture

**Disclaimer:** This is literally entirely **AI generated**, it was made purely as an experiment and **proof-of-concept**.
Use at your own risk.

Captures the Steam Frame eye camera feeds and hosts them in a format accepted by EyeTrackVR.

Capture is achieved by borrowing the DMA-BUF file
descriptors that XRService fills.

See [Here](/EXPLAINED.MD) for an AI generated explanation of how this works to base your own projects off this.

# How to Use

Upload this repo to the Frame, run install-service.sh

Stream URLs for EyeTrackVR should now be available at:
http://<frame IP>:8090/1
http://<frame IP>:8090/0

**You do this at your own risk.**
This repo is here only as a proof of concept, I'd highly advise against using this if you're not familiar with what you're doing or how to fix any problems that may arise!

# Troubleshooting

Eye camera streams are blank?
Put on the headset. The eye tracking service stops when the headset isn't on, thus no buffers to find for the stream.

Eye camera streams appear wrong?
Take off the headset for a few seconds, put it back on.