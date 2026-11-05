// Матрица «категория × месяц» за 36 месяцев (change add-category-month-analytics)
// против /expense-structure за тот же период — одна VU-итерация = два запроса:
// /api/reports/expense-by-category-by-month (tag expense-by-category-by-month) и
// /api/reports/expense-structure без сравнения (tag expense-structure-36m).
// Критерий задачи 5.1: p95 матрицы не более чем втрое выше p95 структуры за
// тот же период — сравнивается по http_req_duration{endpoint:...} в summary.json.
//
// Период — 36 месяцев назад от сегодня: шире 730-дневного засева
// seed_loadtest по умолчанию, то есть захватывает всю историю леджера (худший
// случай для матрицы — все категории во всех непустых месяцах).
//
// Запуск через record_run.sh:
//   SCENARIO_FILE=scenario_category_by_month.js CLIENT_LEVELS="10 30" bash record_run.sh
import http from 'k6/http';
import { check } from 'k6';
import { pickToken } from './lib/tokens.js';
import { SUMMARY_OPTIONS } from './lib/summary_options.js';

const BASE_URL = __ENV.GATEWAY_URL || 'http://localhost:8091';
const VUS = Number(__ENV.VUS || 10);
const DURATION = __ENV.DURATION || '1m';

export const options = {
    ...SUMMARY_OPTIONS,
    scenarios: {
        category_by_month: {
            executor: 'constant-vus',
            vus: VUS,
            duration: DURATION,
        },
    },
    thresholds: {
        http_req_failed: ['rate<0.01'],
        // Пустые пороги — только чтобы k6 вывел метрику по каждому тегу
        // отдельно в summary.json.
        'http_req_duration{endpoint:expense-by-category-by-month}': ['max>=0'],
        'http_req_duration{endpoint:expense-structure-36m}': ['max>=0'],
    },
};

function isoDateTime(d) {
    return d.toISOString().slice(0, 19); // без "Z", тот же формат, что у lib/endpoints.js
}

const TO = new Date();
const FROM = new Date(Date.UTC(TO.getUTCFullYear() - 3, TO.getUTCMonth(), 1));

export default function () {
    const headers = { Authorization: `Bearer ${pickToken()}` };
    const period = `from=${isoDateTime(FROM)}&to=${isoDateTime(TO)}&eq_currency=USD`;

    const requests = [
        ['expense-by-category-by-month', `${BASE_URL}/api/reports/expense-by-category-by-month?${period}`],
        ['expense-structure-36m', `${BASE_URL}/api/reports/expense-structure?${period}`],
    ];
    for (const [endpoint, url] of requests) {
        const res = http.get(url, { headers, tags: { endpoint } });
        check(res, {
            [`${endpoint}: status 200`]: (r) => r.status === 200,
        });
    }
}
