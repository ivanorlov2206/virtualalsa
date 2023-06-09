# Virtual ALSA driver
## How it works?
This small driver uses platform_driver and platform_device interfaces to
make the DMA buffer allocation possible, as well as ALSA snd_\* api to
actually generate some sound. You can probably use this driver as an
example of virtual ALSA driver for your own purposes, or for some userspace applications testing/fuzzing.

It creates the virtual sound card called "pcmtest". After inserting the module
you will be able to find it in aplay -L list.

## What can it do?
It can:

- Simulate capture and playback modes
- Generate random or pattern-based capture data
- Simulate up to 8 substreams, 4 channels
- Support interleaved and non-interleaved access modes
- Inject errors into the PCM callbacks
- Inject delays into the capturing process

```
arecord -D hw:CARD=pcmtest,DEV=0 -c 1 -f S16_LE --duration=3 out.wav
```
And you have 3 seconds of beautiful white noise...

The driver itself has two modes for capture data generating:

0. The random sequence of bytes
1. The pattern repeating mode

To change the module mode, write the corresponding option to the module parameter:
```
echo 1 > /sys/module/pcmtest/parameters/fill_mode
```
The most interesting is the second mode, where you can specify the pattern to repeat:
```
echo some_pattern > /sys/kernel/debug/pcmtest/fill_pattern
```
The pattern can be up to 4096 bytes long.

Also, it can be used for checking the playback functionality.
If the playback buffer contains the looped pattern (which you set in fill_pattern) the test will
success, and the
```
/sys/kernel/debug/pcmtest/pc_test
```
file will contain '1' after the pcm closing. Otherwise, if the buffer is
corrupted somehow, this debugfs file will contain '0'.

If you want to test the playback functionality as mentioned above, the pattern must not contain
zeros - otherwise the test results will be incorrect.
## Reset IOCTL redefinition
This driver can be used to test the 'RESET' ioctl redefinition through ALSA API. To test it, reset
the pcm (for example, with snd_pcm_reset call), and check this debugfs file (in case if the new
IOCTL triggers, it will contain '1', otherwise - '0').

## Errors and delays injecting
The module has several parameters, which can help you to inject errors into the PCM callbacks and
inject delays into the playback and capturing processes.

See module parameters inside pcmtest.c
