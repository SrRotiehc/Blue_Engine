#!/usr/bin/env bash

set -u
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
SDL_CFLAGS="${SDL_CFLAGS-$(pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)}"
SDL_LIBS="${SDL_LIBS-$(pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null || echo -lSDL2)}"
GL_LIBS="${GL_LIBS--lGL}"

if [ ! -f BlueEngine.cpp ] || [ ! -f main.py ]; then
    echo "Erro: BlueEngine.cpp and main.py need to be on the same folder as build.sh" >&2
    exit 1
fi

if [ $# -gt 0 ]; then PYTHONS=("$@"); else PYTHONS=(python3.11 python3.12 python3.13 python3.14); fi

ok=0; fail=0
for PY in "${PYTHONS[@]}"; do
    if ! command -v "$PY" >/dev/null 2>&1; then
        echo "[ignoring] $PY not found"; fail=$((fail+1)); continue
    fi
    if ! INCLUDES="$("$PY" -m pybind11 --includes 2>/dev/null)"; then
        echo "[pulando] $PY sem pybind11 (rode: $PY -m pip install pybind11)"; fail=$((fail+1)); continue
    fi
    SUFFIX="$("$PY" -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')"
    OUT="BlueEngine$SUFFIX"

    echo "[compiling] $PY -> $OUT"
    # shellcheck disable=SC2086
    if $CXX -O2 -std=c++17 -shared -fPIC -fvisibility=hidden \
            $INCLUDES $SDL_CFLAGS BlueEngine.cpp -o "$OUT" $SDL_LIBS $GL_LIBS; then
        ok=$((ok+1))
    else
        echo "[error] failed in $PY" >&2; fail=$((fail+1))
    fi
done

echo "Ready: $ok compiled, $fail with problem."
[ "$fail" -eq 0 ]
