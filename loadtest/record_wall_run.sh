#!/usr/bin/env bash
# Записывает ОДИН прогон scenario_find_the_wall.js в ту же Grafana Perf
# History БД, что и record_run.sh (infra/db/migrations/perf/,
# infra/db/migrations/perf/000002_wall_scenario.up.sql, дашборд
# infra/monitoring/grafana/dashboards/perf-history.json).
#
# В отличие от record_run.sh (sweep — одна k6-команда НА КАЖДЫЙ уровень
# CLIENT_LEVELS, много строк test_result за один test_run) —
# scenario_find_the_wall.js сам нарастает по стадиям (10 -> 1500 rps, см.
# шапку файла) ЗА ОДИН запуск k6, поэтому здесь одна k6-команда и ОДНА
# строка test_result на весь прогон: "clients" в неё — не число VU
# (сценарий open-loop, VU считает сам k6 из maxVUs), а k6-метрика vus_max
# (сколько VU реально понадобилось, чтобы удержать темп) — только для
# traceability. Ключевые метрики этого сценария — новые колонки
# target_rate_max/dropped_iterations (см. миграцию 000002), не "clients".
#
# Требования: те же, что у record_run.sh — стек поднят (включая "db" и
# "prometheus"), миграция perf накачена (bash ../../infra/db/migrate.sh perf up),
# данные засеяны и токены смintены (bash run.sh, см. README.md).
#
# Использование:
#   bash record_wall_run.sh
#   ENVIRONMENT=ci VERSION=1.4.0 BRANCH=main bash record_wall_run.sh
#   MAX_VUS=3000 TARGET_RATE_PEAK=3000 bash record_wall_run.sh   # если поправили stages в самом .js
set -euo pipefail

export MSYS_NO_PATHCONV=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
mkdir -p "$DATA_DIR"

# Контур test (wallet-infra/infra/env/loadtest_env.sh — prod запрещён), как
# в record_run.sh.
# shellcheck source=../../wallet-infra/infra/env/loadtest_env.sh
source "$WORKSPACE_DIR/wallet-infra/infra/env/loadtest_env.sh"

for bin in jq curl git; do
    command -v "$bin" >/dev/null 2>&1 || { echo "Нужен '$bin' на хосте." >&2; exit 1; }
done

# --- Параметры прогона ---
SCENARIO_FILE="${SCENARIO_FILE:-scenario_find_the_wall.js}"
SCENARIO_NAME="${SCENARIO_FILE%.js}"
GATEWAY_URL="${GATEWAY_URL:-http://localhost:8091}"
TOKENS_FILE="${TOKENS_FILE:-$DATA_DIR/tokens.json}"
ENVIRONMENT="${ENVIRONMENT:-$LOADTEST_ENV}"
VERSION="${VERSION:-}"
PROMETHEUS_URL="${PROMETHEUS_URL:-http://localhost:19090}"
# Сервис под нагрузкой — его контейнер в контуре test ищется по id в метке
# cAdvisor "name" (см. PROM_SERVICE в record_run.sh).
PROM_SERVICE="${PROM_SERVICE:-wallet-report}"
PRE_ALLOCATED_VUS="${PRE_ALLOCATED_VUS:-200}"
MAX_VUS="${MAX_VUS:-1500}"
# Пиковый target rate из stages scenario_find_the_wall.js (см. комментарий
# в шапке файла и в миграции 000002) — НЕ читается из .js автоматически,
# держите в синхроне при правке stages там.
TARGET_RATE_PEAK="${TARGET_RATE_PEAK:-1500}"

if [[ ! -f "$TOKENS_FILE" ]]; then
    echo "Не найден $TOKENS_FILE — сначала: bash run.sh (см. README.md, Шаги 1-2)." >&2
    exit 1
fi

if command -v podman >/dev/null 2>&1; then
    ENGINE=podman
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
else
    echo "Не нашёл ни podman, ни docker в PATH." >&2
    exit 1
fi

commit_sha="$(git -C "$SCRIPT_DIR" rev-parse HEAD)"
branch="${BRANCH:-$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD)}"

echo "==> commit=$commit_sha branch=$branch сценарий=$SCENARIO_NAME maxVUs=$MAX_VUS targetPeak=$TARGET_RATE_PEAK"

db_container="$LOADTEST_DB_CONTAINER"

use_native_k6=0
if command -v k6 >/dev/null 2>&1; then
    use_native_k6=1
    echo "==> k6: нативный (найден в PATH)"
else
    echo "==> k6: нет в PATH — гоняю контейнером docker.io/grafana/k6 (см. README.md, Шаг 3)"
    db_network="$LOADTEST_NETWORK"
fi

# То же subquery-усреднение, что в record_run.sh — см. комментарий там же
# (avg_over_time(...) снаружи ОБЯЗАТЕЛЕН: без него /api/v1/query отдаёт
# resultType "matrix" вместо "vector", и .value ниже был бы всегда пуст —
# то есть без этой обёртки avg_cpu_pct/avg_mem_bytes писались бы NULL
# ВСЕГДА, даже при доступном Prometheus).
prom_avg_over_window() {
    local subquery_expr="$1" end_ts="$2"
    local result
    result="$(curl -fsS -G "$PROMETHEUS_URL/api/v1/query" \
        --data-urlencode "query=avg_over_time($subquery_expr)" \
        --data-urlencode "time=$end_ts" 2>/dev/null | jq -r '.data.result[0].value[1] // empty')" || true
    if [[ -z "$result" ]]; then
        echo "null"
    else
        printf '%.2f' "$result"
    fi
}

summary_path="$DATA_DIR/perf_${SCENARIO_NAME}.json"
echo "==> Прогон (все стадии в одном k6-вызове, см. шапку .js) -> $summary_path"

start_ts="$(date +%s)"
if [[ "$use_native_k6" == "1" ]]; then
    GATEWAY_URL="$GATEWAY_URL" TOKENS_FILE="$TOKENS_FILE" \
        PRE_ALLOCATED_VUS="$PRE_ALLOCATED_VUS" MAX_VUS="$MAX_VUS" \
        k6 run --summary-export="$summary_path" "$SCRIPT_DIR/k6/$SCENARIO_FILE"
else
    "$ENGINE" run --rm --network "$db_network" --user "$(id -u):$(id -g)" \
        -v "$SCRIPT_DIR/k6:/scripts:ro" -v "$DATA_DIR:/data" \
        -e GATEWAY_URL="http://wallet-gateway:8090" -e TOKENS_FILE="/data/$(basename "$TOKENS_FILE")" \
        -e PRE_ALLOCATED_VUS="$PRE_ALLOCATED_VUS" -e MAX_VUS="$MAX_VUS" \
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
# vus_max — k6-гейдж "сколько VU реально понадобилось" (см. шапку файла) —
# только traceability, не сравнивайте его между билдами как "уровень".
# test_result.clients — integer, а k6 иногда отдаёт гейджи как "45.0" —
# округляем явно, чтобы psql не упал на "invalid input syntax for integer".
vus_max="$(jq -r '(.metrics.vus_max.value // 0) | round' "$summary_path")"
dropped="$(jq -r '.metrics.dropped_iterations.count // 0' "$summary_path")"

duration_secs=$((end_ts - start_ts))
prom_container_id="$(loadtest_container_id "$PROM_SERVICE")"

echo "    requests=$requests rps=$rps p50=$p50 p90=$p90 p95=$p95 p99=$p99 err=$err vus_max=$vus_max dropped=$dropped"

cpu_pct="$(prom_avg_over_window \
    "(sum(rate(container_cpu_usage_seconds_total{name=\"${prom_container_id}\"}[1m]))*100)[${duration_secs}s:15s]" \
    "$end_ts")"
mem_bytes="$(prom_avg_over_window \
    "(container_memory_usage_bytes{name=\"${prom_container_id}\"})[${duration_secs}s:15s]" \
    "$end_ts")"

parameters_json="$(jq -n --arg scenario "$SCENARIO_FILE" \
    --argjson pre_allocated_vus "$PRE_ALLOCATED_VUS" --argjson max_vus "$MAX_VUS" \
    --argjson target_rate_peak "$TARGET_RATE_PEAK" \
    '{scenario_file: $scenario, pre_allocated_vus: $pre_allocated_vus, max_vus: $max_vus, target_rate_peak: $target_rate_peak}')"

SQL_FILE="$(mktemp)"
trap 'rm -f "$SQL_FILE"' EXIT
{
    echo "INSERT INTO test_run (commit_sha, branch, environment, version, scenario, parameters)"
    echo "VALUES (:'run_commit', :'run_branch', :'run_env', NULLIF(:'run_version', ''), :'run_scenario', :'run_params'::jsonb)"
    echo "RETURNING id \\gset run_"
    echo
    echo "INSERT INTO test_result (test_run_id, scenario, clients, duration_s, requests, rps, p50_ms, p90_ms, p95_ms, p99_ms, error_rate, avg_cpu_pct, avg_mem_bytes, target_rate_max, dropped_iterations)"
    echo "VALUES (:'run_id', :'run_scenario', $vus_max, $duration_secs, $requests, $rps, $p50, $p90, $p95, $p99, $err, $cpu_pct, $mem_bytes, $TARGET_RATE_PEAK, $dropped);"
} > "$SQL_FILE"

echo "==> Пишу в perf_history"
"$ENGINE" exec -i "$db_container" psql -v ON_ERROR_STOP=1 \
    --username "$PERF_POSTGRES_USER" --dbname "$PERF_POSTGRES_DB" \
    -v run_commit="$commit_sha" -v run_branch="$branch" -v run_env="$ENVIRONMENT" \
    -v run_version="$VERSION" -v run_scenario="$SCENARIO_NAME" -v run_params="$parameters_json" \
    < "$SQL_FILE"

echo "==> Готово. Grafana: http://localhost:13000 -> дашборд \"Perf History\" (scenario=$SCENARIO_NAME, branch=$branch)."
