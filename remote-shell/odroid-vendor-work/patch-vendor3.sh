#!/bin/sh
# patch-vendor3.sh - rename the driver's hmac_sha256 (conflicts with kernel crypto/sha2.h)
set -e
cd /usr/src/rtl88x2bu-git

for f in core/crypto/sha256.h core/crypto/sha256.c core/crypto/sha256-prf.c core/rtw_swcrypto.c; do
  [ -f "$f.orig" ] || cp "$f" "$f.orig"
  cp "$f.orig" "$f"
  # \bhmac_sha256\b must NOT match hmac_sha256_vector / hmac_sha256_kdf
  sed -i -E 's/\bhmac_sha256\b/rtw_hmac_sha256/g' "$f"
done
echo "=== renamed occurrences ==="
grep -rn 'rtw_hmac_sha256' core/crypto/sha256.h | head
echo "=== verify no bare hmac_sha256 left ==="
grep -rnE '\bhmac_sha256\b' core/ || echo "none"

echo "=== build ==="
make clean >/dev/null 2>&1 || true
make -j2 ARCH=arm64 KSRC=/lib/modules/$(uname -r)/build CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT= > /root/netbackup/vendor-build11.log 2>&1
echo "RC=$?"
echo "=== distinct errors ==="
grep -oE 'error: [^,;]*' /root/netbackup/vendor-build11.log | sed 's/[0-9]\+//g' | sort | uniq -c | sort -rn | head -15
echo "=== .ko ==="
find . -name '*.ko' 2>/dev/null || echo none
