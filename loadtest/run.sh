#!/usr/bin/env bash
# Готовит нагрузочный контур целиком одним запуском:
#   1. собирает wallet_report_loadtest_seed + mint_loadtest_token (один раз —
#      повторные запуски переиспользуют build-loadtest/, если исходники не
#      менялись);
#   2. сеет тестовые данные в dev-базу уже поднятого стека
#      (../tests_integration/seed_loadtest.cpp);
#   3. минтит токены (../../google-auth/tools/mint_loadtest_token.cpp).
#
# После — сразу k6 (см. README.md, "Шаг 3").
#
# Требует: контур test поднят (wallet-infra/infra/env/up.sh test). Сид,
# токены и нагрузка — только в test (wallet-infra/infra/env/loadtest_env.sh:
# prod запрещён); его db, сеть и ключ подписи берутся оттуда же.
set -euo pipefail

# MSYS (Git Bash на Windows) переписывает абсолютные unix-пути в аргументах
# в Windows-пути ещё до того, как они попадут в podman — тот же приём, что и
# в infra/db/migrate.sh / tests_integration/run_local.sh.
export MSYS_NO_PATHCONV=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WR_DIR="$WORKSPACE_DIR/wallet-report"
GA_DIR="$WORKSPACE_DIR/google-auth"
DATA_DIR="$SCRIPT_DIR/data"
mkdir -p "$DATA_DIR"

# shellcheck source=../../wallet-infra/infra/env/loadtest_env.sh
source "$WORKSPACE_DIR/wallet-infra/infra/env/loadtest_env.sh"
PEM_PATH="$LOADTEST_PEM"

# Движок контейнеров — podman (изначальная разработка) или docker (на этой
# машине podman не установлен вовсе) — берём тот, что реально есть в PATH.
if command -v podman >/dev/null 2>&1; then
    ENGINE=podman
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
else
    echo "Не нашёл ни podman, ни docker в PATH." >&2
    exit 1
fi

db_network="$LOADTEST_NETWORK"

# build-loadtest/ собирается в именованный docker volume, а не прямо в
# смонтированные $WR_DIR/$GA_DIR — репозиторий часто лежит на NTFS/exFAT
# (fuseblk), которая не хранит бит "исполняемый" для обычных файлов:
# бинарник там выходит -rw-r--r--, и даже `chmod +x` внутри контейнера
# эффекта не даёт (permission bit молча не меняется — реальный случай,
# и вручную, и в Jenkins: "Permission denied" на exec только что собранного
# бинаря). Volume хранится в собственном сторадже движка контейнеров
# (обычно ext4/overlay2 хоста), где exec-бит работает как положено —
# исходники и остальной CMake-кэш остаются на обычном bind-mount, переезжает
# только build-loadtest/.
WR_BUILD_VOL="wallet_report_loadtest_build"
GA_BUILD_VOL="google_auth_loadtest_build"

need_build=1
if "$ENGINE" volume inspect "$WR_BUILD_VOL" >/dev/null 2>&1 && \
   "$ENGINE" volume inspect "$GA_BUILD_VOL" >/dev/null 2>&1; then
    if "$ENGINE" run --rm \
        -v "$WR_BUILD_VOL:/wrbin" -v "$GA_BUILD_VOL:/gabin" \
        debian:bookworm-slim sh -c \
        'test -x /wrbin/tests_integration/wallet_report_loadtest_seed && test -x /gabin/mint_loadtest_token'; then
        need_build=0
    fi
fi
[[ "${LOADTEST_FORCE_REBUILD:-}" == "1" ]] && need_build=1

if [[ "$need_build" == "1" ]]; then
    echo "==> Собираю wallet_report_loadtest_seed + mint_loadtest_token (первый раз — дольше, apt-get)"
    "$ENGINE" run --rm \
        -v "$WR_DIR:/wr" \
        -v "$GA_DIR:/ga" \
        -v "$WR_BUILD_VOL:/wr/build-loadtest" \
        -v "$GA_BUILD_VOL:/ga/build-loadtest" \
        debian:bookworm-slim bash -c '
        set -e
        apt-get update -qq
        apt-get install -y --no-install-recommends -qq build-essential cmake libssl-dev libpq-dev zlib1g-dev pkg-config >/dev/null

        cd /wr
        cmake -B build-loadtest -DWALLET_REPORT_BUILD_LOADTEST_SEED=ON >/tmp/wr_cfg.log 2>&1 \
            || { cat /tmp/wr_cfg.log; exit 1; }
        cmake --build build-loadtest --target wallet_report_loadtest_seed -j"$(nproc)"

        cd /ga
        cmake -B build-loadtest -DGOOGLE_AUTH_BUILD_TESTS=OFF -DGOOGLE_AUTH_BUILD_LOADTEST_TOOLS=ON >/tmp/ga_cfg.log 2>&1 \
            || { cat /tmp/ga_cfg.log; exit 1; }
        cmake --build build-loadtest --target mint_loadtest_token -j"$(nproc)"
        '
else
    echo "==> Инструменты уже собраны (docker volume) — LOADTEST_FORCE_REBUILD=1 для пересборки"
fi

# --- Сеять данные — контейнер в сети "db", подключается на db:5432 ролью
#     WALLET_POSTGRES_USER (пишущая, НЕ wallet_reader). ---
echo "==> Сею тестовые данные (LOADTEST_USERS=${LOADTEST_USERS:-5}, LOADTEST_OPS_PER_USER=${LOADTEST_OPS_PER_USER:-3000})"
"$ENGINE" run --rm --network "$db_network" \
    -v "$WR_DIR:/wr" \
    -v "$WR_BUILD_VOL:/wr/build-loadtest" \
    -e DATABASE_URL="postgresql://${WALLET_POSTGRES_USER}:${WALLET_POSTGRES_PASSWORD}@db:5432/${WALLET_POSTGRES_DB}" \
    -e LOADTEST_USERS="${LOADTEST_USERS:-5}" \
    -e LOADTEST_OPS_PER_USER="${LOADTEST_OPS_PER_USER:-3000}" \
    -e LOADTEST_DAYS_SPAN="${LOADTEST_DAYS_SPAN:-730}" \
    -e LOADTEST_USERS_JSON="/wr/loadtest/data/users.json" \
    -e LOG_LEVEL=info \
    -w /wr \
    debian:bookworm-slim bash -c '
        apt-get update -qq && apt-get install -y --no-install-recommends -qq libpq5 >/dev/null
        exec /wr/build-loadtest/tests_integration/wallet_report_loadtest_seed
    '

# --- Минтить токены — своей сети не нужно, только приватный ключ + jq. ---
echo "==> Минтю токены (TTL=${TTL_SECS:-86400}с)"
"$ENGINE" run --rm \
    -v "$WR_DIR:/wr" \
    -v "$GA_DIR:/ga" \
    -v "$GA_BUILD_VOL:/ga/build-loadtest" \
    -v "$PEM_PATH:/key.pem:ro" \
    -e TTL_SECS="${TTL_SECS:-86400}" \
    debian:bookworm-slim bash -c '
    set -e
    apt-get update -qq && apt-get install -y --no-install-recommends -qq jq libssl3 >/dev/null

    users=/wr/loadtest/data/users.json
    out=/wr/loadtest/data/tokens.json
    tmp="$out.tmp"
    echo "[]" > "$tmp"

    count=$(jq "length" "$users")
    for i in $(seq 0 $((count - 1))); do
        user_id=$(jq -r ".[$i].user_id" "$users")
        email=$(jq -r ".[$i].email" "$users")
        token=$(/ga/build-loadtest/mint_loadtest_token --pem /key.pem \
            --sub "$user_id" --email "$email" --ttl-secs "$TTL_SECS")
        jq --arg uid "$user_id" --arg tok "$token" ". + [{\"user_id\": \$uid, \"token\": \$tok}]" "$tmp" > "$tmp.next"
        mv "$tmp.next" "$tmp"
        echo "Токен для $email готов ($((i + 1))/$count)"
    done
    mv "$tmp" "$out"
    '

echo "==> Готово: $DATA_DIR/users.json, $DATA_DIR/tokens.json"
echo "==> Дальше: k6 (см. README.md, Шаг 3) — например, проверить руками:"
echo "    T=\$(grep -o '\"token\": *\"[^\"]*\"' \"$DATA_DIR/tokens.json\" | head -1 | sed -E 's/.*\"([^\"]+)\"\$/\\1/')"
echo "    curl -H \"Authorization: Bearer \$T\" http://localhost:8091/api/reports/currencies"
