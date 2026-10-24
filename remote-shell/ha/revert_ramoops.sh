#!/bin/sh
# Removes the ramoops overlay again (reverses deploy_ramoops.sh).
#
# The reserved RAM region only exists while the overlay is applied, so a reboot
# after this is enough to free it. Nothing else was changed permanently.
set -eu

ENV=/boot/armbianEnv.txt
DTBO=/boot/overlay-user/ramoops.dtbo

echo '== 1. Overlay entfernen =='
rm -f "$DTBO" && echo "entfernt: $DTBO"

echo '== 2. "ramoops" aus user_overlays nehmen (lowfreq bleibt) =='
if grep -q '^user_overlays=' "$ENV"; then
  awk '/^user_overlays=/ {
         split($0, a, "=");
         v = a[2];
         out = "";
         m = split(v, w, " ");
         for (i = 1; i <= m; i++) {
           if (w[i] != "ramoops" && w[i] != "") out = (out == "" ? w[i] : out " " w[i]);
         }
         if (out == "") next;
         print "user_overlays=" out;
         next;
       }
       { print }' "$ENV" > "$ENV.tmp"
  mv "$ENV.tmp" "$ENV"
  grep -n '^user_overlays=' "$ENV" || echo 'user_overlays: keine Eintraege mehr'
fi

echo '== 3. Modul-Load-Eintrag entfernen =='
rm -f /etc/modules-load.d/ramoops.conf && echo 'ramoops.conf entfernt'

sync
echo '== fertig - Reboot noetig =='
