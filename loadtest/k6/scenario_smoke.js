// Ворота перед серьёзной нагрузкой: 1 VU, каждый /api/reports/* эндпоинт по
// разу — проверяет, что токен проходит и через wallet-gateway, и через
// wallet-report (JWT валиден дважды, см. README, "Разбор пути запроса"), и
// что данные вообще есть (200 + валидный JSON, а не пустая заглушка на
// незасеянном леджере).
//
// Запуск: см. ../README.md.
import http from 'k6/http';
import { check } from 'k6';
import { pickToken } from './lib/tokens.js';
import { buildReportRequests } from './lib/endpoints.js';
import { SUMMARY_OPTIONS } from './lib/summary_options.js';

const BASE_URL = __ENV.GATEWAY_URL || 'http://localhost:8090';

export const options = {
    ...SUMMARY_OPTIONS,
    vus: 1,
    iterations: 1,
    thresholds: {
        http_req_failed: ['rate==0'],
    },
};

export default function () {
    const token = pickToken();
    const requests = buildReportRequests(BASE_URL, token);

    for (const [name, req] of Object.entries(requests)) {
        const res = http.request(req.method, req.url, null, req.params);
        check(res, {
            [`${name}: status 200`]: (r) => r.status === 200,
            [`${name}: valid json`]: (r) => {
                try {
                    JSON.parse(r.body);
                    return true;
                } catch (e) {
                    return false;
                }
            },
        });
        if (res.status !== 200) {
            console.error(`${name} -> ${res.status}: ${res.body}`);
        }
    }
}
