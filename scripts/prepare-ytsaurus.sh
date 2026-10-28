#!/usr/bin/env bash
# Готовит дерево ytsaurus для сборки сервисов:
#  1. клон ytsaurus на зафиксированном коммите (patches/ytsaurus.commit);
#  2. применение patches/ytsaurus.patch (TLS поверх открытого соединения,
#     отображение ошибок в gRPC status codes);
#  3. musl и его зависимости из yandex/yatool (patches/yatool.commit) для
#     статической сборки.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
YT_ROOT="${YT_ROOT:-$ROOT/third_party/ytsaurus}"
YATOOL_ROOT="${YATOOL_ROOT:-$ROOT/third_party/yatool-musl}"

YT_COMMIT="$(cat "$ROOT/patches/ytsaurus.commit")"
YATOOL_COMMIT="$(cat "$ROOT/patches/yatool.commit")"
mapfile -t VENDORED_DIRS < <(grep -vE '^\s*(#|$)' "$ROOT/patches/yatool-vendored-dirs.txt")

fetch_commit() {
    local url="$1" dir="$2" commit="$3"
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$commit"
}

# ytsaurus
if [ ! -d "$YT_ROOT/.git" ]; then
    fetch_commit https://github.com/ytsaurus/ytsaurus.git "$YT_ROOT" "$YT_COMMIT"
    git -C "$YT_ROOT" checkout -q FETCH_HEAD
fi
if git -C "$YT_ROOT" apply --check "$ROOT/patches/ytsaurus.patch" 2>/dev/null; then
    git -C "$YT_ROOT" apply "$ROOT/patches/ytsaurus.patch"
    echo "Applied ytsaurus.patch"
elif git -C "$YT_ROOT" apply --check --reverse "$ROOT/patches/ytsaurus.patch" 2>/dev/null; then
    echo "ytsaurus.patch is already applied"
else
    echo "ytsaurus.patch does not apply cleanly" >&2
    exit 1
fi

# musl из yatool
if [ ! -d "$YATOOL_ROOT/.git" ]; then
    git init -q "$YATOOL_ROOT"
    git -C "$YATOOL_ROOT" remote add origin https://github.com/yandex/yatool.git
    git -C "$YATOOL_ROOT" sparse-checkout init --no-cone
fi
git -C "$YATOOL_ROOT" sparse-checkout set --no-cone "${VENDORED_DIRS[@]}"
git -C "$YATOOL_ROOT" fetch -q --depth 1 --filter=blob:none origin "$YATOOL_COMMIT"
git -C "$YATOOL_ROOT" checkout -q FETCH_HEAD

for dir in "${VENDORED_DIRS[@]}"; do
    rm -rf "${YT_ROOT:?}/$dir"
    mkdir -p "$(dirname "$YT_ROOT/$dir")"
    cp -r "$YATOOL_ROOT/$dir" "$YT_ROOT/$dir"
done
echo "Vendored from yatool: ${VENDORED_DIRS[*]}"
