# Virtual ALSA driver
## How it works?
This small driver uses platform_driver and platform_device interfaces to
make the DMA buffer allocation possible, as well as ALSA snd_\* api to
actually generate some sound. You can probably use this driver as an
example of virtual ALSA driver for your own purposes.

It creates the virtual sound card called "alsav". After inserting the module
you will be able to find it in aplay -L list.

## What can it do?
```
arecord -D hw:CARD=alsav,DEV=0 -c 1 -f S16_LE --duration=3 out.wav
```
And you have 3 seconds of beautiful white noise...

The driver itself has three modes for pattern generating:
0. The random sequence of bytes
1. The looped monotonically increasing sequence of bytes (0, 1, 2, ..., FF, 0, 1, 2, ...)
2. The pattern repeating mode

To change the module mode, write the corresponding option to the module parameter:
```
echo 1 > /sys/module/alsav/parameters/fill_mode
```
The most interesting is the third mode, where you can specify the pattern to repeat:
```
echo some_pattern > /sys/module/alsav/parameters/fill_pattern
```

Also, it can be used for checking the playback functionality.
If the playback buffer contains the looped monotonically increasing sequence of
bytes (0, 1, 2, ..., FF, 0, 1, ...) the test will success, and the
```
/sys/kernel/debug/alsav/pc_test
```
file will contain '1' after the pcm closing. Otherwise, if the buffer is
corrupted somehow, this debugfs file will contain '0'.
