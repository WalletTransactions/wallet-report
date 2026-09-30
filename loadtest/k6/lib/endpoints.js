// Полный список GET /api/reports/* (см. wallet-report/openapi.yaml)
// с реалистичными query-параметрами — общий для всех сценариев, чтобы не
// дублировать список эндпоинтов в каждом .js по отдельности.

function isoDate(d) {
    return d.toISOString().slice(0, 10); // YYYY-MM-DD
}

function isoDateTime(d) {
    return d.toISOString().slice(0, 19); // YYYY-MM-DDTHH:mm:ss — без "Z", тот же формат, что у seed_loadtest
}

function daysAgo(n) {
    const d = new Date();
    d.setUTCDate(d.getUTCDate() - n);
    return d;
}

function qs(params) {
    return Object.entries(params)
        .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
        .join('&');
}

// Окно "сейчас" — последний год данных, плюс год ДО него для cmp_from/cmp_to
// (expense-structure, expense-biggest-moves) — оба окна внутри 730-дневного
// диапазона, который засевает seed_loadtest по умолчанию, поэтому оба
// возвращают непустые данные, а не 0 строк.
const TO = new Date();
const FROM = daysAgo(365);
const CMP_TO = daysAgo(366);
const CMP_FROM = daysAgo(730);

// baseUrl — БЕЗ финального "/" (localhost:8090 или http://wallet-gateway:8090).
// token — Bearer JWT одного синтетического пользователя (см. lib/tokens.js).
// Возвращает { <имя>: {method, url, params} } — готово и для http.request
// (по одному), и для http.batch (Object.values(...), см. scenario_dashboard_burst.js).
export function buildReportRequests(baseUrl, token) {
    const headers = { Authorization: `Bearer ${token}` };
    const tag = (endpoint) => ({ headers, tags: { endpoint } });

    const to = isoDateTime(TO);
    const from = isoDateTime(FROM);
    const cmpTo = isoDateTime(CMP_TO);
    const cmpFrom = isoDateTime(CMP_FROM);
    const dateOnly = isoDate(TO);

    return {
        balance: {
            method: 'GET',
            url: `${baseUrl}/api/reports/balance?${qs({ date: dateOnly, eq_currency: 'USD' })}`,
            params: tag('balance'),
        },
        income: {
            method: 'GET',
            url: `${baseUrl}/api/reports/income?${qs({ from, to, eq_currency: 'USD' })}`,
            params: tag('income'),
        },
        expense: {
            method: 'GET',
            url: `${baseUrl}/api/reports/expense?${qs({ from, to, eq_currency: 'USD' })}`,
            params: tag('expense'),
        },
        expense_structure: {
            method: 'GET',
            url: `${baseUrl}/api/reports/expense-structure?${qs({
                from,
                to,
                eq_currency: 'USD',
                cmp_from: cmpFrom,
                cmp_to: cmpTo,
            })}`,
            params: tag('expense-structure'),
        },
        income_expense_by_month: {
            method: 'GET',
            url: `${baseUrl}/api/reports/income-expense-by-month?${qs({ from, to, eq_currency: 'USD' })}`,
            params: tag('income-expense-by-month'),
        },
        expense_by_weekday: {
            method: 'GET',
            url: `${baseUrl}/api/reports/expense-by-weekday?${qs({ from, to, eq_currency: 'USD' })}`,
            params: tag('expense-by-weekday'),
        },
        expense_biggest_moves: {
            method: 'GET',
            url: `${baseUrl}/api/reports/expense-biggest-moves?${qs({
                from,
                to,
                eq_currency: 'USD',
                cmp_from: cmpFrom,
                cmp_to: cmpTo,
                limit: 6,
            })}`,
            params: tag('expense-biggest-moves'),
        },
        operations: {
            method: 'GET',
            url: `${baseUrl}/api/reports/operations?${qs({ from, to, eq_currency: 'USD' })}`,
            params: tag('operations'),
        },
        wallet_turnover: {
            method: 'GET',
            url: `${baseUrl}/api/reports/wallet-turnover?${qs({ from, to, eq_currency: 'USD' })}`,
            params: tag('wallet-turnover'),
        },
        balance_at_dates: {
            method: 'GET',
            url: `${baseUrl}/api/reports/balance-at-dates?${qs({
                dates: `${isoDate(FROM)},${dateOnly}`,
                eq_currency: 'USD',
            })}`,
            params: tag('balance-at-dates'),
        },
        // Самый тяжёлый эндпоинт (пересчитывает бакеты с начала истории
        // ледгера на каждый счёт заново, см. комментарий к db::Pool в
        // src/db.hpp) — тот, что назван причиной завести пул вообще.
        balance_series: {
            method: 'GET',
            url: `${baseUrl}/api/reports/balance-series?${qs({ from, to, eq_currency: 'USD', step: 'month' })}`,
            params: tag('balance-series'),
        },
        currencies: {
            method: 'GET',
            url: `${baseUrl}/api/reports/currencies`,
            params: tag('currencies'),
        },
    };
}
