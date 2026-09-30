#!/usr/bin/env bash
# Достаёт числовые метрики из --summary-export JSON k6 в CSV, который
# понимает Jenkins Plot Plugin (wallet-report/loadtest/jenkins/
# Jenkinsfile, стадия "Report") — Plot Plugin сам копит историю точек по
# билдам, этому скрипту нужно отдать РОВНО одну строку данных на билд, не
# вести накопительный файл самому.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Использование: $0 <summary.json> <out.csv>" >&2
    exit 2
fi

SUMMARY="$1"
OUT="$2"

if [[ ! -f "$SUMMARY" ]]; then
    echo "Не найден файл: $SUMMARY" >&2
    exit 2
fi

p95=$(jq -r '.metrics.http_req_duration."p(95)" // 0' "$SUMMARY")
avg=$(jq -r '.metrics.http_req_duration.avg // 0' "$SUMMARY")
rate=$(jq -r '.metrics.http_reqs.rate // 0' "$SUMMARY")
err=$(jq -r '.metrics.http_req_failed.value // 0' "$SUMMARY")

{
    echo "p95_ms,avg_ms,reqs_per_sec,error_rate"
    printf "%.2f,%.2f,%.2f,%.4f\n" "$p95" "$avg" "$rate" "$err"
} > "$OUT"
