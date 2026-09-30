// Грузит data/tokens.json (см. ../../mint_tokens.sh) через SharedArray —
// одна копия на процесс k6, а не на VU (см. k6/data, SharedArray) — и
// раздаёт токен каждому VU по кругу: у каждого VU один и тот же токен на
// весь прогон, т.е. один VU == один синтетический пользователь, открывший
// вкладку "Отчёты".
import { SharedArray } from 'k6/data';

const TOKENS_FILE = __ENV.TOKENS_FILE || './data/tokens.json';

export const tokens = new SharedArray('tokens', function () {
    const raw = JSON.parse(open(TOKENS_FILE));
    if (!Array.isArray(raw) || raw.length === 0) {
        throw new Error(
            `${TOKENS_FILE} пуст или не найден — сначала прогоните seed_loadtest и mint_tokens.sh ` +
                '(см. wallet-report/loadtest/README.md)'
        );
    }
    return raw;
});

export function pickToken() {
    const idx = (__VU - 1) % tokens.length;
    return tokens[idx].token;
}
