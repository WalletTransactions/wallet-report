// Лента операций за год с серверными фильтрами (change add-operations-filters)
// против той же ленты без фильтров — одна VU-итерация = три запроса
// /api/reports/operations за последний год: без фильтров (tag operations),
// с q (operations-q) и с category_uuid (operations-category). Критерий задачи
// 4.1: p95 с фильтром не более чем вдвое выше p95 без фильтров того же периода
// — сравнивается по http_req_duration{endpoint:...} в summary.json.
//
// q по умолчанию — подстрока, которой нет в засеве seed_loadtest (там у всех
// операций комментарий "loadtest seed"): худший случай для фильтра —
// просмотр всех операций периода без единого совпадения, плюс второй запрос
// итога. Q=seed — обратный край: совпадают все, страница по 1000 плюс итог.
// category_uuid — первая категория леджера каждого пользователя, берётся в
// setup() из /api/reports/accounts?kind=category (по одному запросу на токен).
//
// Запуск через record_run.sh:
//   SCENARIO_FILE=scenario_operations_filters.js CLIENT_LEVELS="10 30" bash record_run.sh
import http from 'k6/http';
import { check } from 'k6';
import { tokens, pickToken } from './lib/tokens.js';
import { buildReportRequests } from './lib/endpoints.js';
import { SUMMARY_OPTIONS } from './lib/summary_options.js';

const BASE_URL = __ENV.GATEWAY_URL || 'http://localhost:8091';
const VUS = Number(__ENV.VUS || 10);
const DURATION = __ENV.DURATION || '1m';
const Q = __ENV.Q || 'такси';

export const options = {
    ...SUMMARY_OPTIONS,
    scenarios: {
        operations_filters: {
            executor: 'constant-vus',
            vus: VUS,
            duration: DURATION,
        },
    },
    thresholds: {
        http_req_failed: ['rate<0.01'],
        // Пустые пороги — только чтобы k6 вывел метрику по каждому тегу
        // отдельно в summary.json.
        'http_req_duration{endpoint:operations}': ['max>=0'],
        'http_req_duration{endpoint:operations-q}': ['max>=0'],
        'http_req_duration{endpoint:operations-category}': ['max>=0'],
    },
};

// Индекс токена -> uuid первой категории его леджера ("" — категорий нет).
export function setup() {
    return tokens.map((t) => {
        const res = http.get(`${BASE_URL}/api/reports/accounts?kind=category`, {
            headers: { Authorization: `Bearer ${t.token}` },
            tags: { endpoint: 'setup-accounts' },
        });
        if (res.status !== 200) return '';
        const accounts = JSON.parse(res.body).accounts || [];
        return accounts.length ? accounts[0].uuid : '';
    });
}

export default function (categories) {
    const token = pickToken();
    const base = buildReportRequests(BASE_URL, token).operations;
    const headers = base.params.headers;
    const category = categories[(__VU - 1) % tokens.length];

    const requests = [
        ['operations', base.url],
        ['operations-q', `${base.url}&q=${encodeURIComponent(Q)}`],
    ];
    if (category) requests.push(['operations-category', `${base.url}&category_uuid=${category}`]);

    for (const [endpoint, url] of requests) {
        const res = http.get(url, { headers, tags: { endpoint } });
        check(res, {
            [`${endpoint}: status 200`]: (r) => r.status === 200,
            [`${endpoint}: итог только при фильтре`]: (r) => {
                try {
                    return ('total_eq' in JSON.parse(r.body)) === (endpoint !== 'operations');
                } catch (e) {
                    return false;
                }
            },
        });
    }
}
