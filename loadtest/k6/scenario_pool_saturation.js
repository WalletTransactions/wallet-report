// Сустейн-нагрузка ИМЕННО на balance-series — самый тяжёлый эндпоинт
// (пересчитывает бакеты с начала истории на каждый счёт заново, см.
// комментарий к db::Pool в wallet-report/src/db.hpp) — при разном
// числе одновременных VU, в т.ч. заметно больше REPORTS_DB_POOL_SIZE.
// Именно этот сценарий воспроизводит "один тяжёлый отчёт блокирует всех
// остальных" — то, что чинил переход с одного соединения на пул.
//
// VUS/DURATION — параметры прогона, не сам фикс: гоняйте этот файл
// несколько раз с разными VUS при REPORTS_DB_POOL_SIZE=1, затем при =8, =16...
// (см. ../README.md, "Методика").
//
// Этот же параметр VUS делает файл готовым sweep-сценарием для
// record_run.sh (../record_run.sh, "Performance DB" — история прогонов в
// Postgres, сравнение baseline/current в Grafana): CLIENT_LEVELS перебирает
// значения VUS одно за другим, каждое — свой summary.json, все — один
// test_run с несколькими test_result (см. README, "Запись в Performance DB").
import http from 'k6/http';
import { check } from 'k6';
import { pickToken } from './lib/tokens.js';
import { buildReportRequests } from './lib/endpoints.js';
import { SUMMARY_OPTIONS } from './lib/summary_options.js';

const BASE_URL = __ENV.GATEWAY_URL || 'http://localhost:8090';
const VUS = Number(__ENV.VUS || 20);
const DURATION = __ENV.DURATION || '2m';

export const options = {
    ...SUMMARY_OPTIONS,
    scenarios: {
        balance_series_sustained: {
            executor: 'constant-vus',
            vus: VUS,
            duration: DURATION,
        },
    },
    thresholds: {
        http_req_failed: ['rate<0.01'],
    },
};

export default function () {
    const token = pickToken();
    const req = buildReportRequests(BASE_URL, token).balance_series;
    const res = http.request(req.method, req.url, null, req.params);
    check(res, { 'balance-series: status 200': (r) => r.status === 200 });
}
