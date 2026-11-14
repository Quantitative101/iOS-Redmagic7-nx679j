#!/bin/bash
# Print Unix time minus the PMK8350 RTC count. The count is read from the phone running Android (root); the time is
# the Mac's (Android's own clock can be wrong: it lost its time when the RTC was reset, 2026-11-14). The iOS kernel
# adds this to the RTC to get the date (mkpayload.py --rtc-offset). Valid until the RTC is reset again: run it for
# every image.
S=${1:-${RM_SERIAL:?export RM_SERIAL=<your adb serial>}}
rtc=$(adb -s "$S" shell "su -c 'cat /sys/class/rtc/rtc0/since_epoch'" | tr -d '\r')
now=$(date +%s)
[ -n "$rtc" ] || { echo "no reading" >&2; exit 1; }
echo $((now - rtc))
