#!/usr/bin/env bash
# Sign cs2_128tick.exe with the Casio certificate on Linux (apt install osslsigncode).
# The .pfx is PRIVATE - keep it outside this repo.
#   CASIO_PFX=~/keys/casio.pfx CASIO_PFX_PASS=... ./sign.sh
set -euo pipefail
cd "$(dirname "$0")"
: "${CASIO_PFX:?set CASIO_PFX}"; : "${CASIO_PFX_PASS:?set CASIO_PFX_PASS}"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
openssl pkcs12 -in "$CASIO_PFX" -passin env:CASIO_PFX_PASS -nokeys -out "$tmp/c.crt"
openssl pkcs12 -in "$CASIO_PFX" -passin env:CASIO_PFX_PASS -nocerts -nodes -out "$tmp/c.key"
osslsigncode sign -certs "$tmp/c.crt" -key "$tmp/c.key" -n "CS2 128-Tick Patcher" \
  -i "https://github.com/Casio-Byte" -h sha256 -in cs2_128tick.exe -out "$tmp/signed.exe"
mv "$tmp/signed.exe" cs2_128tick.exe
osslsigncode verify -CAfile "$tmp/c.crt" -in cs2_128tick.exe | grep -E "Subject|Signature verification"
