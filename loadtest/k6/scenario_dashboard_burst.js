// Главный сценарий — воспроизводит ровно то, что описано в комментарии к
// db::Pool (wallet-report/src/db.hpp): один показ вкладки "Отчёты"
// на клиенте — это ~8-10 параллельных GET-запросов разом, не по очереди.
// Одна VU-итерация здесь = одно такое открытие (все эндпоинты из
// lib/endpoints.js запущены разом через http.batch). Ступени VU (= сколько
// "открытий" одновременно) растут, чтобы увидеть, где начинается
// деградация.
//
// Порог по balance-series (самый тяжёлый эндпоинт, см. lib/endpoints.js) —
// стартовое число ниже НЕ взято с потолка навсегда: после первого
// baseline-прогона (REPORTS_DB_POOL_SIZE=1, см. README) поправьте его на
// реальный p95 оттуда, иначе порог либо всегда проходит, либо всегда падает.
//
// Запуск и как менять REPORTS_DB_POOL_SIZE между прогонами: см. ../README.md.
import http from 'k6/http';
import { check } from 'k6';
import { pickToken } from './lib/tokens.js';
import { buildReportRequests } from './lib/endpoints.js';
import { SUMMARY_OPTIONS } from './lib/summary_options.js';

const BASE_URL = __ENV.GATEWAY_URL || 'http://localhost:8090';

export const options = {
    ...SUMMARY_OPTIONS,
    stages: [
        { duration: '30s', target: 1 },
        { duration: '1m', target: 10 },
        { duration: '1m', target: 30 },
        { duration: '1m', target: 60 },
        { duration: '30s', target: 0 },
    ],
    thresholds: {
        http_req_failed: ['rate<0.01'],
        'http_req_duration{endpoint:balance-series}': ['p(95)<3000'], // уточнить по первому baseline-прогону
    },
};

export default function () {
    const token = pickToken();
    const requests = buildReportRequests(BASE_URL, token);

    const responses = http.batch(Object.values(requests));
    responses.forEach((res) => {
        check(res, { 'status 200': (r) => r.status === 200 });
    });
}
