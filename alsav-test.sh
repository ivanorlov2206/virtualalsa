#!/bin/bash

uid=$(id -u)
if [ $uid -ne 0 ]; then
	echo "$0: Must be run as root"
	exit 1
fi

if ! which modprobe > /dev/null 2>&1; then
	echo "$0: You need modprobe installed"
        exit 4
fi

if ! modinfo alsav > /dev/null 2>&1; then
	echo "$0: You must have the following enabled in your kernel:"
	echo "CONFIG_SND_ALSAV=m"
	exit 4
fi

systemctl --user stop pulseaudio.socket
systemctl --user stop pulseaudio.service

modprobe alsav

if [ ! -e /sys/kernel/debug/alsav/pc_test ]; then
	mount -t debugfs none /sys/kernel/debug

	if [ ! -e /sys/kernel/debug/alsav/pc_test ]; then
		echo "$0: Error mounting debugfs"
		exit 4
	fi
fi

arecord -D hw:CARD=alsav,DEV=0 -c 1 -f S16_LE -r 8000 --duration=4 out.wav
aplay -D hw:CARD=alsav,DEV=0 -c 1 -f S16_LE -r 8000 out.wav
test_val = $(< /sys/kernel/debug/alsav/pc_test)

if [["$test_val" -eq "1"]]
	then echo "Success"
else
	then echo "Fail"
fi

rm -rf out.wav
rmmod alsav

systemctl --user start pulseaudio.socket
systemctl --user start pulseaudio.service
