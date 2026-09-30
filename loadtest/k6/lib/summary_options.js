// Общий набор percentile-ов для --summary-export JSON, шарится всеми
// сценариями (scenario_smoke.js/scenario_dashboard_burst.js/
// scenario_pool_saturation.js) через `...SUMMARY_OPTIONS` в их export const
// options — единственное место, где решается, какие percentile-ы вообще
// попадут в summary.json.
//
// k6 по умолчанию кладёт в summary только avg/min/med/max/p(90)/p(95) —
// БЕЗ p(50) (у него как раз есть "med", то же самое значение, но под другим
// ключом) и БЕЗ p(99) вовсе. record_run.sh (см. ../../record_run.sh) читает
// из summary.json именно "p(50)"/"p(90)"/"p(95)"/"p(99)" — четыре
// одинаковых по форме ключа, а не смесь "med"/"p(90)"/"p(95)" — поэтому
// summaryTrendStats здесь явно перечисляет p(50), а не полагается на "med".
export const SUMMARY_OPTIONS = {
    summaryTrendStats: ['avg', 'min', 'med', 'p(50)', 'p(90)', 'p(95)', 'p(99)', 'max'],
};
