#!/bin/bash
# Turns the end of build.log into one error annotation that shows up on the
# run summary page (readable without opening the raw logs).
log=${1:-build.log}
[ -f "$log" ] || exit 0
{
  echo "=== compiler / linker errors ==="
  grep -E "(: (fatal )?error [A-Z]+[0-9]+|error MSB[0-9]+|error LNK[0-9]+|CMake Error|ERROR:|Exception:)" "$log" \
    | sed -E 's/ \[[^]]*\.vcxproj\]$//' | awk '!seen[$0]++' | head -n 45
  echo "=== last 20 lines ==="
  tail -n 20 "$log"
} > report.txt
msg=$(sed -e 's/%/%25/g' -e 's/\r//g' report.txt | awk 'BEGIN{ORS="%0A"} {print}')
echo "::${3:-error} title=${2:-Build} log::${msg}"
