#!/bin/sh
# Rebuilds examples/prebuilt/*.aci.wasm from the sources (needs clang with the wasm32 target).
set -e
cd "$(dirname "$0")"
mkdir -p prebuilt
CLANG=${AICONTACT_CLANG:-clang}
for src in c/*.c; do
    name=$(basename "$src" .c)
    "$CLANG" --target=wasm32 -O2 -nostdlib -fno-exceptions -Wall -Wno-unused-function -Wno-unused-parameter \
        -Wl,--no-entry -Wl,-z,stack-size=65536 -Wl,--strip-debug \
        -I ../sdk/c -o "prebuilt/$name.aci.wasm" "$src"
    if [ -d "c/$name.ui" ]; then  # custom UI (index.html or ui.json + assets) -> aci.ui/* sections
        "${PYTHON:-python3}" ../skill/aicontact/scripts/aicontact.py ui pack "prebuilt/$name.aci.wasm" "c/$name.ui" > /dev/null
    fi
    echo "built prebuilt/$name.aci.wasm"
done
"${PYTHON:-python3}" -c "import sys, wasmtime; open(sys.argv[2], 'wb').write(wasmtime.wat2wasm(open(sys.argv[1]).read()))" \
    wat/minimal.wat prebuilt/wat-square.aci.wasm && echo "built prebuilt/wat-square.aci.wasm"
