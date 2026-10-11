#!/bin/sh
# Run on the board after uploading this script and S99zzzvideo (LF endings).
# Only install/patch the video entrypoints; do not rerun network/module init.
set -eu
SOURCE=${1:-./S99zzzvideo}
SERVICE=/etc/init.d/S99zzzvideo
LAUNCH=/oem/usr/bin/RkLunch.sh
STOP=/oem/usr/bin/RkLunch-stop.sh
BACKUP=/oem/usr/share/video-service-original
[ -f "$SOURCE" ] && [ -f "$LAUNCH" ] && [ -f "$STOP" ]
sh -n "$SOURCE"
mkdir -p "$BACKUP"
[ -f "$BACKUP/RkLunch.sh" ] || cp -p "$LAUNCH" "$BACKUP/RkLunch.sh"
[ -f "$BACKUP/RkLunch-stop.sh" ] || cp -p "$STOP" "$BACKUP/RkLunch-stop.sh"
[ ! -f "$SERVICE" ] || [ -f "$BACKUP/S99zzzvideo" ] || cp -p "$SERVICE" "$BACKUP/S99zzzvideo"
trap 'rm -f "$LAUNCH.video-new" "$STOP.video-new" "$SERVICE.video-new"' EXIT

if grep -Fq '# luckfox-video-service: managed startup' "$LAUNCH"; then
    cp "$LAUNCH" "$LAUNCH.video-new"
else
    # Refuse a different vendor layout instead of guessing what to replace.
    awk '
    /^[\t ]*if \[ -d "\/oem\/usr\/share\/iqfiles" \]; then$/ {
        if (getline <= 0 || $0 !~ /^[\t ]*rkipc -a \/oem\/usr\/share\/iqfiles &$/) exit 2
        if (getline <= 0 || $0 !~ /^[\t ]*else$/) exit 2
        if (getline <= 0 || $0 !~ /^[\t ]*rkipc &$/) exit 2
        if (getline <= 0 || $0 !~ /^[\t ]*fi$/) exit 2
        print "\t# luckfox-video-service: managed startup"
        print "\t/etc/init.d/S99zzzvideo start"
        found++; next
    }
    { print }
    END { if (found != 1) exit 2 }
    ' "$LAUNCH" > "$LAUNCH.video-new"
fi
if grep -Fq '# luckfox-video-service: managed shutdown' "$STOP"; then
    cp "$STOP" "$STOP.video-new"
else
    # Stop the supervisor before the old killall/wait sequence can restart it.
    awk '
    /^killall rkipc$/ {
        print "# luckfox-video-service: managed shutdown"
        print "if [ -x /etc/init.d/S99zzzvideo ]; then"
        print "    /etc/init.d/S99zzzvideo stop || exit 1"
        print "fi"
        found++
    }
    { print }
    END { if (found != 1) exit 2 }
    ' "$STOP" > "$STOP.video-new"
fi
sh -n "$LAUNCH.video-new"
sh -n "$STOP.video-new"
cp "$SOURCE" "$SERVICE.video-new"
chmod 755 "$SERVICE.video-new" "$LAUNCH.video-new" "$STOP.video-new"
mv "$SERVICE.video-new" "$SERVICE"
mv "$STOP.video-new" "$STOP"
mv "$LAUNCH.video-new" "$LAUNCH"
"$SERVICE" start
echo "video: installed; original entrypoints are in $BACKUP"
