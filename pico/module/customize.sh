# QuestLHSync install (PICO 4 Pro): lhsight reads the tracking cameras inside pvrtrackingservice
MODEL=$(getprop ro.product.model)
FW=$(getprop ro.build.display.id)
case "$MODEL" in
  *A8110*|*"PICO 4 Pro"*|*"Pico 4 Pro"*) ui_print "- PICO 4 Pro ($MODEL, $FW)" ;;
  *) ui_print "! this is $MODEL, not a PICO 4 Pro: lhsight may not find its cameras" ;;
esac
[ -r /mnt/vendor/persist/pvr/mav0/cam0/sensor.yaml ] || ui_print "! no camera calibration in /mnt/vendor/persist/pvr/mav0"
# keep a camera map changed on this headset over an update (not the first builds' unchecked "0,1,2,3")
OLD=/data/adb/modules/questlhsync_pico/cammap.txt
[ -f "$OLD" ] && [ "$(tr -d ' \r\n' < "$OLD")" != "0,1,2,3" ] && cp -f "$OLD" "$MODPATH/cammap.txt"
set_perm "$MODPATH/lhsyncd" 0 0 0755
set_perm "$MODPATH/frida-inject" 0 0 0755
ui_print "- lhsyncd starts at boot. To start it now:"
ui_print "  su -c 'setsid sh $MODPATH/service.sh </dev/null >/dev/null 2>&1 &'"
