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

# Креды мигратора и wallet_reader — того кластера, в который идём. У контуров wallet-<env>
# они в секретах деплоя (~/.config/wallet-secrets/<env>/db.env, как у wallet-infra
# migrate.sh); infra/db/.env — только у локального dev-стека. Раньше это не всплывало:
# docker exec psql идёт через сокет (trust), а миграции и тесты — без TLS. Тот же приём,
# что в wallet-api/tests_integration/run_local.sh. prod — отказ до чтения его секретов.
IT_DB_PROJECT="${IT_DB_PROJECT:-wallet-test}"
if [[ "$IT_DB_PROJECT" == "wallet-prod" ]]; then
    echo "IT_DB_PROJECT=wallet-prod запрещён: интеграционные тесты — только не в prod-кластере." >&2
    exit 1
fi
secrets_env="${WALLET_SECRETS_ROOT:-$HOME/.config/wallet-secrets}/${IT_DB_PROJECT#wallet-}/db.env"
if [[ -n "${IT_DB_ENV_FILE:-}" ]]; then
    ENV_FILE="$IT_DB_ENV_FILE"
elif [[ "$IT_DB_PROJECT" == wallet-* && -f "$secrets_env" ]]; then
    ENV_FILE="$secrets_env"
else
    ENV_FILE="$WORKSPACE_DIR/wallet-infra/infra/db/.env"
fi
if [[ ! -f "$ENV_FILE" ]]; then
    echo "Не найден $ENV_FILE — сначала: cp infra/db/.env.example infra/db/.env (или IT_DB_ENV_FILE=...)" >&2
    exit 1
fi
echo "==> Креды: $ENV_FILE"
set -a
source "$ENV_FILE"
set +a

# --- Найти запущенный контейнер "db" (тот же приём, что в migrate.sh: имя
#     контейнера зависит от имени папки проекта, лейбл — нет; лейбл зависит
#     от того, какой compose-provider реально исполнял `podman compose`). ---
# На хосте два кластера — wallet-prod и wallet-test (wallet-infra/infra/env):
# без фильтра по проекту `head -n1` мог взять prod и завести тестовую базу и
# миграции в prod-кластере. Только test; prod — отказ.
db_container="$("$ENGINE" ps --filter "label=io.podman.compose.service=db" --filter "label=io.podman.compose.project=$IT_DB_PROJECT" --format '{{.Names}}' | head -n1)"
if [[ -z "$db_container" ]]; then
    db_container="$("$ENGINE" ps --filter "label=com.docker.compose.service=db" --filter "label=com.docker.compose.project=$IT_DB_PROJECT" --format '{{.Names}}' | head -n1)"
fi
if [[ -z "$db_container" ]]; then
    echo "Не нашёл запущенный контейнер 'db' проекта '$IT_DB_PROJECT' (IT_DB_PROJECT=...)." >&2
    exit 1
fi
echo "==> Кластер: $db_container (проект $IT_DB_PROJECT)"
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

# TLS к базе (wallet-infra, change add-internal-mtls, 9.1): при mTLS контура pg_hba
# отклоняет подключения без шифрования (hostnossl reject), клиентский сертификат не
# нужен — хватает sslmode=require. Режим берём у самого кластера: ssl=off — как раньше.
db_ssl="$("$ENGINE" exec -e PGPASSWORD="$MIGRATOR_POSTGRES_PASSWORD" "$db_container" \
    psql -U "$MIGRATOR_POSTGRES_USER" -d postgres -tAc "SHOW ssl")"
IT_DB_SSLMODE="${IT_DB_SSLMODE:-$([[ "$db_ssl" == "on" ]] && echo require || echo disable)}"
echo "==> TLS к базе: ssl=$db_ssl, sslmode=$IT_DB_SSLMODE"
# Пароль роли wallet_reader: роли общие на кластер, и wallet-infra migrate.sh задаёт его
# из READER_POSTGRES_PASSWORD контура; литерал из миграции — только у dev-стека.
READER_PASSWORD="${READER_POSTGRES_PASSWORD:-wallet_reader}"

echo "==> Миграции (идемпотентно — golang-migrate накатывает только новые)"
"$ENGINE" run --rm --network "$db_network" \
    -v "$WORKSPACE_DIR/wallet-api/migrations:/migrations:ro" \
    docker.io/migrate/migrate -path=/migrations \
    -database "postgres://${MIGRATOR_POSTGRES_USER}:${MIGRATOR_POSTGRES_PASSWORD}@db:5432/${IT_DB_NAME}?sslmode=${IT_DB_SSLMODE}" \
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
    -e DATABASE_URL="postgres://${MIGRATOR_POSTGRES_USER}:${MIGRATOR_POSTGRES_PASSWORD}@db:5432/${IT_DB_NAME}?sslmode=${IT_DB_SSLMODE}" \
    -e REPORTS_DATABASE_URL="postgres://wallet_reader:${READER_PASSWORD}@db:5432/${IT_DB_NAME}?sslmode=${IT_DB_SSLMODE}" \
    -e JUNIT_XML_PATH=/results/junit.xml \
    "$TEST_IMAGE"
