#!/bin/bash
# Wait for the phone to come back (fastboot after Power+Vol Down, or adb), return it to slot b, pull and decode the ring.
N=$1
S=${RM_SERIAL:?export RM_SERIAL=<your adb serial>}
cd ${MIGA_ROOT:-$HOME/MakeiOSGreatAgain}/redmagic7
while true; do
  if fastboot devices | grep -q $S; then
    fastboot -s $S set_active b && fastboot -s $S reboot
    for t in 1 2 3; do sleep 20; fastboot devices | grep -q $S || break; fastboot -s $S reboot; done
    break
  fi
  if adb devices | grep -q "$S.*device"; then break; fi
  sleep 3
done
adb -s $S wait-for-device
until [ "$(adb -s $S shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 3; done
sleep 5
adb -s $S shell su -c "dd if=/dev/block/by-name/userdata bs=4096 skip=$((36446404608/4096-1025)) count=1025 2>/dev/null" > logs/ring$N.bin
python3 ../ref/redmagic/ios-mondrian/tools/decode-mondrian-log.py logs/ring$N.bin > logs/ring$N.txt 2>&1
wc -l logs/ring$N.txt
