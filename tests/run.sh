#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root_dir"

make -s

fail=0
mkdir -p tests/out

for case_file in tests/cases/*.c; do
    base=$(basename "$case_file" .c)
    out_file="tests/out/${base}.i"
    expected_file="tests/expected/${base}.i"

    ./jcpp "$case_file" > "$out_file"

    if diff -u "$expected_file" "$out_file"; then
        echo "PASS: $base"
    else
        echo "FAIL: $base"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi
