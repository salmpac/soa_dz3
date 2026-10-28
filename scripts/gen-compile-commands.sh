#!/usr/bin/env bash
# Генерирует compile_commands.json для clangd (VS Code и т.п.).
#
# ya собирает код из копии src/ внутри ytsaurus (third_party/ytsaurus/soa),
# поэтому база переписывается так, чтобы clangd работал с настоящими файлами
# в src/:
#  - записи soa/** указывают на src/**;
#  - #include <soa/...> резолвится через .build/src-overlay/soa -> src.
# Сгенерированные заголовки (*.pb.h и т.п.) лежат в .build/ya-out.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
YT_ROOT="${YT_ROOT:-$ROOT/third_party/ytsaurus}"
OUT="$ROOT/.build/ya-out"
OVERLAY="$ROOT/.build/src-overlay"

if [ "$#" -eq 0 ]; then
    # Все программы проекта (каталоги с PROGRAM в ya.make).
    mapfile -t TARGETS < <(cd "$ROOT/src" && grep -l '^PROGRAM' -r --include=ya.make . | xargs -n1 dirname | sed 's|^\./|soa/|' | sort)
else
    TARGETS=("$@")
fi

rsync -a --delete "$ROOT/src/" "$YT_ROOT/soa/"

mkdir -p "$OUT" "$OVERLAY"
ln -sfn ../../src "$OVERLAY/soa"

cd "$YT_ROOT"
# Сборка нужна ради сгенерированных заголовков в $OUT.
./ya make -r -DMUSL=yes --output="$OUT" --add-protobuf-result --no-src-links "${TARGETS[@]}"
./ya dump compile-commands -r -DMUSL=yes --cmd-build-root="$OUT" --dont-strip-compiler-path "${TARGETS[@]}" \
    > "$ROOT/.build/compile_commands.raw.json"

python3 - "$ROOT" "$YT_ROOT" "$OVERLAY" <<'PY'
import json
import sys

root, yt_root, overlay = sys.argv[1:]
soa_copy = yt_root + "/soa/"
src = root + "/src/"

with open(root + "/.build/compile_commands.raw.json") as f:
    entries = json.load(f)

result = []
for entry in entries:
    path = entry["file"]
    # Исходники contrib (grpc, openssl, ...) не индексируем: навигация по их
    # заголовкам работает и так, а фоновая индексация сильно быстрее.
    if path.startswith(yt_root + "/contrib/"):
        continue
    if path.startswith(soa_copy):
        entry["file"] = src + path[len(soa_copy):]

    args = entry.get("arguments") or entry["command"].split()
    patched = []
    for arg in args:
        if arg == "-I" + yt_root:
            patched.append("-I" + overlay)
        if arg == "-Werror":
            continue
        patched.append(arg.replace(soa_copy, src) if arg.startswith(soa_copy) else arg)
    entry.pop("command", None)
    entry["arguments"] = patched
    result.append(entry)

with open(root + "/compile_commands.json", "w") as f:
    json.dump(result, f, indent=1)
print(f"compile_commands.json: {len(result)} entries")
PY
