// НЕ ещё один sustained-прогон, как scenario_pool_saturation.js (closed-loop:
// фиксированное число VU, каждый ждёт свой ответ и только потом шлёт
// следующий запрос — при росте латентности реальная нагрузка на сервер сама
// падает, "просадку" им физически не увидеть, только линейный рост p95 с
// плато по throughput).
//
// Здесь — open-loop 'ramping-arrival-rate': k6 держит ЗАДАННЫЙ темп запросов
// в секунду независимо от того, как быстро сервер отвечает (сам поднимает
// столько VU из maxVUs, сколько нужно, чтобы держать target rate). Именно
// так реально находят потолок: наращиваем rate по стадиям, пока сервер не
// перестанет успевать — тогда fail rate/dropped_iterations уходят от нуля,
// p95/p99 улетают в секунды, а фактический achieved rate отрывается от
// target вниз. Это тот самый "просело", который не получить constant-vus.
//
// На одной машине под сервер и нагрузку сразу: смотрите на CPU самого k6 в
// cAdvisor (дашборд "Wallet Transactions — Overview") рядом с CPU
// wallet-gateway/wallet-report/db — если ляжет k6, а не сервер, это k6 стал
// узким местом, не находка про сервер (тогда — снижайте потолок stages или
// поднимайте maxVUs/CPU k6).
import http from 'k6/http';
import { check } from 'k6';
import { pickToken } from './lib/tokens.js';
import { buildReportRequests } from './lib/endpoints.js';
import { SUMMARY_OPTIONS } from './lib/summary_options.js';

const BASE_URL = __ENV.GATEWAY_URL || 'http://localhost:8090';
const PRE_ALLOCATED_VUS = Number(__ENV.PRE_ALLOCATED_VUS || 200);
const MAX_VUS = Number(__ENV.MAX_VUS || 1500);

export const options = {
    ...SUMMARY_OPTIONS,
    scenarios: {
        find_the_wall: {
            executor: 'ramping-arrival-rate',
            startRate: 10,
            timeUnit: '1s',
            preAllocatedVUs: PRE_ALLOCATED_VUS,
            maxVUs: MAX_VUS,
            stages: [
                { target: 30, duration: '20s' },
                { target: 60, duration: '20s' },
                { target: 120, duration: '20s' },
                { target: 250, duration: '20s' },
                { target: 500, duration: '20s' },
                { target: 1000, duration: '20s' },
                { target: 1500, duration: '20s' },
                { target: 0, duration: '20s' }, // остыть — увидеть, отпускает ли систему после пика
            ],
        },
    },
    // Порогов намеренно нет (thresholds) — цель прогона узнать, ГДЕ ляжет,
    // а не пройти/не пройти CI-гейт; abortOnFail тут был бы вреден.
};

export default function () {
    const token = pickToken();
    const req = buildReportRequests(BASE_URL, token).balance_series;
    const res = http.request(req.method, req.url, null, req.params);
    check(res, { 'balance-series: status 200': (r) => r.status === 200 });
}
