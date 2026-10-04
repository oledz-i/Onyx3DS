#!/bin/bash
# ci-annotate.sh <file> <title> [level]: post a text file as one annotation.
[ -f "$1" ] || exit 0
msg=$(head -c 6000 "$1" | sed -e 's/%/%25/g' -e 's/\r//g' | awk 'BEGIN{ORS="%0A"} {print}')
echo "::${3:-warning} title=${2}::${msg}"
