#!/usr/bin/env bash
# Локальная сборка через ya make.
#
# ya не принимает исходники вне корня репозитория (и разыменовывает симлинки),
# поэтому src/ синхронизируется внутрь дерева ytsaurus как каталог soa/.
# Пути в выводе компилятора переписываются обратно на src/, чтобы ошибки
# открывались в настоящих файлах (в т.ч. по клику в VS Code).
#
# Использование:
#   scripts/build-local.sh                      # все программы из src/
#   scripts/build-local.sh soa/flight_service   # конкретные цели
#
# Бинарники: .build/bin/<имя программы>.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
YT_ROOT="${YT_ROOT:-$ROOT/third_party/ytsaurus}"
OUT="$ROOT/.build/ya-out"
BIN="$ROOT/.build/bin"

if [ "$#" -eq 0 ]; then
    # Все программы проекта (каталоги с PROGRAM в ya.make).
    mapfile -t TARGETS < <(cd "$ROOT/src" && grep -l '^PROGRAM' -r --include=ya.make . | xargs -n1 dirname | sed 's|^\./|soa/|' | sort)
else
    # Цели можно указывать как soa/<dir> или как путь в src/ (src/<dir>, src/<dir>/file.cpp).
    TARGETS=()
    for original in "$@"; do
        arg="${original#./}"
        if [ -f "$ROOT/$arg" ]; then
            arg="$(dirname "$arg")"
        fi
        case "$arg" in
            src) arg="";;
            src/*) arg="${arg#src/}";;
            soa/*) arg="${arg#soa/}";;
        esac
        # Поднимаемся до ближайшего каталога с ya.make.
        while [ -n "$arg" ] && [ ! -f "$ROOT/src/$arg/ya.make" ]; do
            if [ "$arg" = "$(dirname "$arg")" ] || [ "$(dirname "$arg")" = "." ]; then
                arg=""
            else
                arg="$(dirname "$arg")"
            fi
        done
        if [ -z "$arg" ]; then
            echo "No ya.make found for $original" >&2
            exit 1
        fi
        TARGETS+=("soa/$arg")
    done
fi

rsync -a --delete "$ROOT/src/" "$YT_ROOT/soa/"
mkdir -p "$OUT" "$BIN"

cd "$YT_ROOT"
# -DMUSL=yes: статическая линковка с musl, бинарники не зависят от glibc.
# --add-protobuf-result: сгенерированные *.pb.h остаются в $OUT (нужны clangd).
./ya make -r -DMUSL=yes --output="$OUT" --no-src-links --add-protobuf-result "${TARGETS[@]}" 2>&1 \
    | sed -u -E \
        -e 's/\x1b\[[0-9;]*[mK]//g' \
        -e "s#$YT_ROOT/soa/#$ROOT/src/#g" \
        -e "s#\\\$S/soa/#$ROOT/src/#g" \
        -e "s#\\\$\\(S\\)/soa/#$ROOT/src/#g"

# Ссылки на собранные программы в одном месте.
for target in "${TARGETS[@]}"; do
    program="$(sed -nE 's/^PROGRAM\(([^)]*)\).*/\1/p' "$ROOT/src/${target#soa/}/ya.make")"
    if [ -n "$program" ] && [ -f "$OUT/$target/$program" ]; then
        ln -sfn "$OUT/$target/$program" "$BIN/$program"
    fi
done
