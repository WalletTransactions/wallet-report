#!/usr/bin/env bash
# Записывает "Performance DB" — прогоняет k6-сценарий (по умолчанию
# scenario_pool_saturation.js — единственный, параметризованный по числу
# одновременных клиентов через VUS, см. его шапку) один раз НА КАЖДЫЙ
# уровень concurrency из CLIENT_LEVELS, и кладёт результат в Postgres
# (test_run + один test_result на уровень) — infra/db/migrations/perf/,
# читается дашбордом Grafana infra/monitoring/grafana/dashboards/perf-history.json.
#
# В отличие от compare_results.sh (сравнивает РОВНО два summary.json,
# ничего не запоминая) — этот скрипт копит ИСТОРИЮ: baseline/current в
# дашборде выбираются дропдаунами по ветке/коммиту среди уже записанных
# прогонов, не как аргументы скрипта.
#
# Требования: стек поднят (`podman compose --env-file infra/db/.env up -d`,
# включая "db" и "prometheus"), миграция perf накачена
# (`bash ../../infra/db/migrate.sh perf up`), данные засеяны и токены
# смintены (`bash run.sh`, см. README.md).
#
# Использование:
#   bash record_run.sh
#   CLIENT_LEVELS="100 500 1000" DURATION_SECS=60 bash record_run.sh
#   ENVIRONMENT=ci VERSION=1.4.0 bash record_run.sh
#   BRANCH="pool-size-16" POOL_SIZE=16 bash record_run.sh
#     (BRANCH — понятное имя прогона для дропдаунов Baseline/Current в
#     Grafana-дашборде "Perf History": там сравнение идёт РОВНО по этому
#     полю, git-ветка сама по себе для сравнения разных прогонов одного и
#     того же коммита/ветки бесполезна — без override все прогоны легли бы
#     под одним именем и сравнивать было бы не с чем. POOL_SIZE — не более
#     чем для traceability, попадает в parameters, дропдаунов под него нет.)
set -euo pipefail

export MSYS_NO_PATHCONV=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
mkdir -p "$DATA_DIR"

ENV_FILE="$WORKSPACE_DIR/wallet-infra/infra/db/.env"
if [[ ! -f "$ENV_FILE" ]]; then
    echo "Не найден $ENV_FILE — сначала: cp infra/db/.env.example infra/db/.env" >&2
    exit 1
fi
set -a
source "$ENV_FILE"
set +a

for bin in jq curl git; do
    command -v "$bin" >/dev/null 2>&1 || { echo "Нужен '$bin' на хосте." >&2; exit 1; }
done

# --- Параметры прогона ---
SCENARIO_FILE="${SCENARIO_FILE:-scenario_pool_saturation.js}"
SCENARIO_NAME="${SCENARIO_NAME:-${SCENARIO_FILE%.js}}"
CLIENT_LEVELS="${CLIENT_LEVELS:-10 30 60}"
DURATION_SECS="${DURATION_SECS:-30}"
GATEWAY_URL="${GATEWAY_URL:-http://localhost:8090}"
TOKENS_FILE="${TOKENS_FILE:-$DATA_DIR/tokens.json}"
ENVIRONMENT="${ENVIRONMENT:-local-docker-compose}"
VERSION="${VERSION:-}"
PROMETHEUS_URL="${PROMETHEUS_URL:-http://localhost:19090}"
# Подстрока в cAdvisor'овском лейбле "image" (НЕ "name" — с переходом Docker
# на containerd-snapshotter и отключением докеровского обработчика у
# cAdvisor, см. wallet-infra/compose.yaml/cadvisor, "name" стал сырым container
# id, а не человекочитаемым именем; тот же довод, что у панелей
# "Контейнеры" в wallet-overview.json — легенда там тоже по image).
# "servers-wallet-report" в image (не голое "wallet-report") — имя образа
# компоуз собирает из <имя_папки_проекта>-<имя_сервиса>, подстрокой надёжнее
# полного совпадения (то же соображение, что раньше было про cAdvisor "name").
PROM_CONTAINER_MATCH="${PROM_CONTAINER_MATCH:-wallet-report}"
POOL_SIZE="${POOL_SIZE:-}"
# База/роль reports_pool под нагрузкой (см. REPORTS_DATABASE_URL в
# wallet-infra/compose.yaml, сервис "wallet-report") — по ним фильтруются
# pg_stat_activity/pg_stat_database в postgres-exporter, чтобы не мешать в
# одну кучу reports_pool (wallet_reader) с пулом самого wallet-api
# (wallet_wallet_api) на той же базе.
PG_TARGET_DB="${PG_TARGET_DB:-wallet_api}"
PG_TARGET_USER="${PG_TARGET_USER:-wallet_reader}"
# Пусто по умолчанию — CSV с DB-метриками не пишется (локальный sweep по
# нескольким CLIENT_LEVELS не ложится в "ровно одна строка на билд", как
# требует Jenkins Plot Plugin, см. заголовок extract_metrics.sh). Задаётся
# только из Jenkinsfile (см. BuildPlatform/jobs/wallet-report-loadtest/), где
# CLIENT_LEVELS="$VUS" — ровно один уровень, значит и ровно одна строка.
PG_METRICS_CSV="${PG_METRICS_CSV:-}"

if [[ ! -f "$TOKENS_FILE" ]]; then
    echo "Не найден $TOKENS_FILE — сначала: bash run.sh (см. README.md, Шаги 1-2)." >&2
    exit 1
fi

if [[ -n "$PG_METRICS_CSV" && "$(echo "$CLIENT_LEVELS" | wc -w)" != "1" ]]; then
    echo "PG_METRICS_CSV задан, но CLIENT_LEVELS=\"$CLIENT_LEVELS\" — не ровно один уровень." >&2
    echo "Jenkins Plot Plugin ждёт РОВНО одну строку данных на билд (см. комментарий у PG_METRICS_CSV)." >&2
    exit 2
fi

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

commit_sha="$(git -C "$SCRIPT_DIR" rev-parse HEAD)"
# BRANCH override — см. шапку файла: без него все прогоны одного коммита
# легли бы в Grafana под одним и тем же именем ветки и сравнивать было бы
# нечего (см. запрос пользователя — ровно эта проблема).
branch="${BRANCH:-$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD)}"

echo "==> commit=$commit_sha branch=$branch сценарий=$SCENARIO_NAME уровни=[$CLIENT_LEVELS] по ${DURATION_SECS}с"

db_container="$("$ENGINE" ps --filter "label=io.podman.compose.service=db" --format '{{.Names}}' | head -n1)"
if [[ -z "$db_container" ]]; then
    db_container="$("$ENGINE" ps --filter "label=com.docker.compose.service=db" --format '{{.Names}}' | head -n1)"
fi
if [[ -z "$db_container" ]]; then
    echo "Не нашёл запущенный контейнер 'db'. Сначала: $ENGINE compose --env-file infra/db/.env up -d" >&2
    exit 1
fi

use_native_k6=0
if command -v k6 >/dev/null 2>&1; then
    use_native_k6=1
    echo "==> k6: нативный (найден в PATH)"
else
    echo "==> k6: нет в PATH — гоняю контейнером docker.io/grafana/k6 (см. README.md, Шаг 3)"
    db_network="$("$ENGINE" inspect "$db_container" --format '{{range $k, $v := .NetworkSettings.Networks}}{{$k}}{{end}}')"
fi

# avg_over_time( <expr>[<window>s:15s] ) от времени end_ts — subquery
# Prometheus (см. https://prometheus.io/docs/prometheus/latest/querying/basics/#subquery)
# — даёт "среднее за последние <window> секунд ДО end_ts", не привязываясь к
# тому, что сейчас "сейчас" — то, что нужно, раз прогон уже закончился к
# моменту запроса. avg_over_time(...) снаружи ОБЯЗАТЕЛЕН: сама по себе
# subquery-выражение "<expr>[Ns:15s]" без обёртки — range vector (matrix), а
# не instant vector, /api/v1/query отдаёт под неё resultType "matrix" с
# полем .values (массив точек), не .value — .data.result[0].value ниже был
# бы всегда пуст. 15s разрешение подquery — с запасом грубее scrape_interval
# Prometheus (10s, см. infra/monitoring/prometheus.yml), чтобы не
# спотыкаться о ровно один скрейп на грани окна. Принимает ГОТОВОЕ
# subquery-выражение (caller сам приписывает "[${window_secs}s:15s]") — для
# запросов, которые уже сами по себе instant vector (increase()-based
# отношения и т.п.), см. prom_query_at ниже. При недоступном Prometheus/
# пустом результате — echo "null" (JSON null), не 0: NULL в test_result
# честнее нуля (см. комментарий в 000001_init.up.sql).
prom_avg_over_window() {
    local subquery_expr="$1" end_ts="$2"
    prom_query_at "avg_over_time($subquery_expr)" "$end_ts"
}

# Instant-vector PromQL "как есть", без оборачивания в avg_over_time — для
# выражений, которые уже сводятся к одному числу на момент end_ts сами по
# себе (increase(...)/rate(...) с диапазоном в самом выражении, см.
# avg_pg_cache_hit_pct ниже) — обёртка avg_over_time() тут была бы ошибкой
# PromQL ("expected type range vector... got instant vector").
prom_query_at() {
    local promql="$1" end_ts="$2"
    local result
    result="$(curl -fsS -G "$PROMETHEUS_URL/api/v1/query" \
        --data-urlencode "query=$promql" \
        --data-urlencode "time=$end_ts" 2>/dev/null | jq -r '.data.result[0].value[1] // empty')" || true
    if [[ -z "$result" ]]; then
        echo "null"
    else
        printf '%.2f' "$result"
    fi
}

# --- Собрать SQL: один INSERT test_run (RETURNING id \gset), затем один
#     многострочный INSERT test_result — по одной строке на уровень. ---
SQL_FILE="$(mktemp)"
trap 'rm -f "$SQL_FILE"' EXIT

parameters_json="$(jq -n --arg scenario "$SCENARIO_FILE" --arg levels "$CLIENT_LEVELS" \
    --argjson duration_secs "$DURATION_SECS" --arg pool_size "$POOL_SIZE" \
    '{scenario_file: $scenario, client_levels: ($levels | split(" ")), duration_secs: $duration_secs}
      + (if $pool_size != "" then {reports_db_pool_size: ($pool_size | tonumber)} else {} end)')"

{
    echo "INSERT INTO test_run (commit_sha, branch, environment, version, scenario, parameters)"
    echo "VALUES (:'run_commit', :'run_branch', :'run_env', NULLIF(:'run_version', ''), :'run_scenario', :'run_params'::jsonb)"
    echo "RETURNING id \\gset run_"
    echo
    echo "INSERT INTO test_result (test_run_id, scenario, clients, duration_s, requests, rps, p50_ms, p90_ms, p95_ms, p99_ms, error_rate, avg_cpu_pct, avg_mem_bytes, avg_pg_active_conns, avg_pg_cache_hit_pct, avg_pg_lock_waits) VALUES"
} > "$SQL_FILE"

first_row=1
for clients in $CLIENT_LEVELS; do
    summary_path="$DATA_DIR/perf_${SCENARIO_NAME}_${clients}.json"
    echo "==> Уровень clients=$clients (${DURATION_SECS}с) -> $summary_path"

    start_ts="$(date +%s)"
    if [[ "$use_native_k6" == "1" ]]; then
        GATEWAY_URL="$GATEWAY_URL" TOKENS_FILE="$TOKENS_FILE" VUS="$clients" DURATION="${DURATION_SECS}s" \
            k6 run --summary-export="$summary_path" "$SCRIPT_DIR/k6/$SCENARIO_FILE"
    else
        "$ENGINE" run --rm --network "$db_network" --user "$(id -u):$(id -g)" \
            -v "$SCRIPT_DIR/k6:/scripts:ro" \
            -v "$DATA_DIR:/data" \
            -e GATEWAY_URL="http://wallet-gateway:8090" \
            -e TOKENS_FILE="/data/tokens.json" \
            -e VUS="$clients" -e DURATION="${DURATION_SECS}s" \
            docker.io/grafana/k6 run --summary-export="/data/$(basename "$summary_path")" "/scripts/$SCENARIO_FILE"
    fi
    end_ts="$(date +%s)"

    if [[ ! -f "$summary_path" ]]; then
        echo "Не нашёл $summary_path после прогона — k6 упал раньше summary-export?" >&2
        exit 1
    fi

    requests="$(jq -r '.metrics.http_reqs.count // 0' "$summary_path")"
    rps="$(jq -r '.metrics.http_reqs.rate // 0' "$summary_path")"
    err="$(jq -r '.metrics.http_req_failed.value // 0' "$summary_path")"
    p50="$(jq -r '.metrics.http_req_duration."p(50)" // 0' "$summary_path")"
    p90="$(jq -r '.metrics.http_req_duration."p(90)" // 0' "$summary_path")"
    p95="$(jq -r '.metrics.http_req_duration."p(95)" // 0' "$summary_path")"
    p99="$(jq -r '.metrics.http_req_duration."p(99)" // 0' "$summary_path")"

    # Окно — ФАКТИЧЕСКАЯ длительность прогона этого уровня (не всегда ровно
    # DURATION_SECS — старт/стоп k6 и сам вызов curl добавляют секунды),
    # подставляется прямо в subquery-выражение (см. prom_avg_over_window).
    window_secs=$((end_ts - start_ts))
    cpu_pct="$(prom_avg_over_window \
        "(sum(rate(container_cpu_usage_seconds_total{image=~\".*${PROM_CONTAINER_MATCH}.*\"}[1m]))*100)[${window_secs}s:15s]" \
        "$end_ts")"
    mem_bytes="$(prom_avg_over_window \
        "(container_memory_usage_bytes{image=~\".*${PROM_CONTAINER_MATCH}.*\"})[${window_secs}s:15s]" \
        "$end_ts")"

    # Насыщение reports_pool — средняя занятость (не "idle") соединений
    # wallet_reader на wallet_api за окно (см. PG_TARGET_DB/PG_TARGET_USER
    # выше) — прямой сигнал по сценарию scenario_pool_saturation.js.
    pg_active_conns="$(prom_avg_over_window \
        "(sum(pg_stat_activity_count{datname=\"${PG_TARGET_DB}\",usename=\"${PG_TARGET_USER}\",state=~\"active|idle in transaction\"}) or vector(0))[${window_secs}s:15s]" \
        "$end_ts")"

    # Buffer cache hit ratio, % — increase() по ДЕЛЬТЕ counters за окно
    # прогона (не avg_over_time мгновенного отношения — см. комментарий у
    # prom_query_at), clamp_min на случай нулевого знаменателя (нет
    # обращений к БД за окно вовсе).
    pg_cache_hit_pct="$(prom_query_at \
        "100 * sum(increase(pg_stat_database_blks_hit{datname=\"${PG_TARGET_DB}\"}[${window_secs}s])) / clamp_min(sum(increase(pg_stat_database_blks_hit{datname=\"${PG_TARGET_DB}\"}[${window_secs}s])) + sum(increase(pg_stat_database_blks_read{datname=\"${PG_TARGET_DB}\"}[${window_secs}s])), 1)" \
        "$end_ts")"

    # Ожидающие (не granted) блокировки на целевой базе — custom-метрика
    # locks_waiting, отдаётся с лейблом datname (см.
    # infra/monitoring/postgres-exporter/queries.yaml и --extend.query-path
    # в wallet-infra/compose.yaml). sum(...) ОБЯЗАТЕЛЕН перед "or vector(0)" — без
    # него "X or vector(0)" не работает как фолбэк на пустой результат: "or"
    # матчит серии по ПОЛНОМУ набору лейблов, а vector(0) — единственная
    # серия с ПУСТЫМ набором, так что при непустой X с реальными лейблами
    # (datname/instance/job/server) она никогда не совпадёт с vector(0) — obе
    # серии просто складываются В ОДИН результат вместо фолбэка (проверено на
    # этом стенде). sum() стягивает лейблы X в тот же пустой набор, что и у
    # vector(0), и тогда "or" действительно матчит.
    pg_lock_waits="$(prom_avg_over_window \
        "(sum(locks_waiting_count{datname=\"${PG_TARGET_DB}\"}) or vector(0))[${window_secs}s:15s]" \
        "$end_ts")"

    echo "    requests=$requests rps=$rps p50=$p50 p90=$p90 p95=$p95 p99=$p99 err=$err cpu%=$cpu_pct mem=$mem_bytes pg_conns=$pg_active_conns pg_cache_hit%=$pg_cache_hit_pct pg_lock_waits=$pg_lock_waits"

    if [[ -n "$PG_METRICS_CSV" ]]; then
        # Перезаписываем (не дописываем) — тот же приём, что у
        # extract_metrics.sh: Plot Plugin сам копит историю по билдам,
        # этому файлу нужно отдать РОВНО одну строку данных на билд (см.
        # проверку CLIENT_LEVELS выше), не вести накопительный файл самому.
        # "null" (недоступный Prometheus, см. prom_avg_over_window) — валиден
        # для NULL в test_result, но не для Plot Plugin (числовой CSV,
        # свалит ВЕСЬ post{always{}} билда, включая archiveArtifacts — у
        # plot, в отличие от publishHTML, нет allowMissing-аналога), поэтому
        # здесь, только для CSV, подменяем на 0.
        {
            echo "pg_active_conns,pg_cache_hit_pct,pg_lock_waits"
            echo "${pg_active_conns/null/0},${pg_cache_hit_pct/null/0},${pg_lock_waits/null/0}"
        } > "$PG_METRICS_CSV"
    fi

    [[ "$first_row" == "1" ]] && first_row=0 || echo "," >> "$SQL_FILE"
    printf "(:'run_id', :'run_scenario', %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)" \
        "$clients" "$DURATION_SECS" "$requests" "$rps" "$p50" "$p90" "$p95" "$p99" "$err" "$cpu_pct" "$mem_bytes" \
        "$pg_active_conns" "$pg_cache_hit_pct" "$pg_lock_waits" \
        >> "$SQL_FILE"
done
echo ";" >> "$SQL_FILE"

echo "==> Пишу в perf_history (test_run + $(echo "$CLIENT_LEVELS" | wc -w) test_result)"
"$ENGINE" exec -i "$db_container" psql -v ON_ERROR_STOP=1 \
    --username "$PERF_POSTGRES_USER" --dbname "$PERF_POSTGRES_DB" \
    -v run_commit="$commit_sha" -v run_branch="$branch" -v run_env="$ENVIRONMENT" \
    -v run_version="$VERSION" -v run_scenario="$SCENARIO_NAME" -v run_params="$parameters_json" \
    < "$SQL_FILE"

echo "==> Готово. Grafana: http://localhost:13000 -> дашборд \"Perf History\"."
