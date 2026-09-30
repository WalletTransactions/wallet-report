#!/bin/bash
# Прогоняет wallet_report_integration_tests (test_reports.cpp) — один
# рабочий процесс вместо ручных шагов из README.md, "Как запустить".
#
# База берётся НЕ отдельным одноразовым контейнером, а отдельной, специально
# заведённой базой (wallet_api_it) в ТОМ ЖЕ кластере Postgres, что уже поднят
# вашим dev-стеком (wallet-infra/compose.yaml, сервис "db") — переиспользуем сеть
# и сам контейнер, которые и так подняты. Настоящие данные (база
# wallet_api) эта тестовая база не трогает — CREATE DATABASE в том же
# кластере даёт полную изоляцию на уровне файлов/каталогов Postgres, разница
# только в том, что не нужно тянуть и поднимать второй Postgres.
#
# Требования: `podman compose --env-file infra/db/.env up -d db` уже
# выполнен (см. wallet-infra/README.md). Первый запуск дольше — собирает
# тестовый образ; повторные быстрее (слои кэшируются, миграции идемпотентны,
# сами тесты не требуют очистки между запусками — см. README.md,
# "Изоляция" — только растёт число строк в wallet_api_it).
set -euo pipefail

# MSYS (Git Bash на Windows) переписывает абсолютные unix-пути в аргументах
# в Windows-пути ещё до того, как они попадут в podman — тот же приём, что и
# в infra/db/migrate.sh.
export MSYS_NO_PATHCONV=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

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

IT_DB_NAME=wallet_api_it
TEST_IMAGE=wallet-report-it-tests

ENV_FILE="$WORKSPACE_DIR/wallet-infra/infra/db/.env"
if [[ ! -f "$ENV_FILE" ]]; then
    echo "Не найден $ENV_FILE — сначала: cp infra/db/.env.example infra/db/.env" >&2
    exit 1
fi
set -a
source "$ENV_FILE"
set +a

# --- Найти запущенный контейнер "db" (тот же приём, что в migrate.sh: имя
#     контейнера зависит от имени папки проекта, лейбл — нет; лейбл зависит
#     от того, какой compose-provider реально исполнял `podman compose`). ---
db_container="$("$ENGINE" ps --filter "label=io.podman.compose.service=db" --format '{{.Names}}' | head -n1)"
if [[ -z "$db_container" ]]; then
    db_container="$("$ENGINE" ps --filter "label=com.docker.compose.service=db" --format '{{.Names}}' | head -n1)"
fi
if [[ -z "$db_container" ]]; then
    echo "Не нашёл запущенный контейнер 'db'. Сначала: podman compose --env-file infra/db/.env up -d db" >&2
    exit 1
fi
db_network="$("$ENGINE" inspect "$db_container" --format '{{range $k, $v := .NetworkSettings.Networks}}{{$k}}{{end}}')"
if [[ -z "$db_network" ]]; then
    echo "Не нашёл сеть контейнера '$db_container'." >&2
    exit 1
fi

echo "==> База $IT_DB_NAME (заводим, если ещё нет)"
exists="$("$ENGINE" exec -e PGPASSWORD="$MIGRATOR_POSTGRES_PASSWORD" "$db_container" \
    psql -U "$MIGRATOR_POSTGRES_USER" -d postgres -tAc \
    "SELECT 1 FROM pg_database WHERE datname = '${IT_DB_NAME}'")"
if [[ "$exists" != "1" ]]; then
    "$ENGINE" exec -e PGPASSWORD="$MIGRATOR_POSTGRES_PASSWORD" "$db_container" \
        psql -U "$MIGRATOR_POSTGRES_USER" -d postgres -c \
        "CREATE DATABASE ${IT_DB_NAME} OWNER ${MIGRATOR_POSTGRES_USER}"
fi

echo "==> Миграции (идемпотентно — golang-migrate накатывает только новые)"
"$ENGINE" run --rm --network "$db_network" \
    -v "$WORKSPACE_DIR/wallet-api/migrations:/migrations:ro" \
    docker.io/migrate/migrate -path=/migrations \
    -database "postgres://${MIGRATOR_POSTGRES_USER}:${MIGRATOR_POSTGRES_PASSWORD}@db:5432/${IT_DB_NAME}?sslmode=disable" \
    up

echo "==> Сборка тестового образа (см. Containerfile рядом)"
# Здесь, в отличие от volume-спеков выше, аргументы — обычные host-пути без
# контейнерной половины, и их как раз НАДО отдать podman.exe в виде
# "D:/...", иначе он молча плюсует их к текущему диску ("D:\d\Docums\...").
# Поэтому MSYS_NO_PATHCONV снят только для этого вызова — пусть Git Bash
# конвертирует их как обычно.
env -u MSYS_NO_PATHCONV "$ENGINE" build -t "$TEST_IMAGE" \
    -f "$SCRIPT_DIR/Containerfile" "$WORKSPACE_DIR/wallet-report"

echo "==> Тесты"
# JUNIT_XML_PATH — см. test_main.cpp: если задан, раннер пишет туда JUnit XML
# (для Jenkins "junit" step, см. ../../../jobs/wallet-report-integration-tests/Jenkinsfile).
# Контейнер образа тестов без USER (см. Containerfile рядом — root), поэтому
# запись в bind-mount не упирается в права, в отличие от k6-образа.
"$ENGINE" run --rm --network "$db_network" \
    -v "$SCRIPT_DIR:/results" \
    -e DATABASE_URL="postgres://${MIGRATOR_POSTGRES_USER}:${MIGRATOR_POSTGRES_PASSWORD}@db:5432/${IT_DB_NAME}?sslmode=disable" \
    -e REPORTS_DATABASE_URL="postgres://wallet_reader:wallet_reader@db:5432/${IT_DB_NAME}?sslmode=disable" \
    -e JUNIT_XML_PATH=/results/junit.xml \
    "$TEST_IMAGE"
