# Virtual ALSA driver
## How it works?
This small driver uses platform_driver and platform_device interfaces to
make the DMA buffer allocation possible, as well as ALSA snd_\* api to
actually generate some sound. You can probably use this driver as an
example of virtual ALSA driver for your own purposes.

It creates the virtual sound card called "alsav". After inserting the module
you will be able to find it in aplay -L list.

## What it can do?
```
arecord -D hw:CARD=alsav,DEV=0 -c 1 -f S16_LE --duration=3 out.wav
```
And you have 3 seconds of beautiful white noise...
And that's all for now :)
