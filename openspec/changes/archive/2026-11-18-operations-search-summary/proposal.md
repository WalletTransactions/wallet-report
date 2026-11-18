# Proposal

## Why

wallet-mcp (`search_operations`) не может ответить на «сколько на X, когда первый и
последний раз, в каких категориях лежит X, без ложных срабатываний»: `/api/reports/operations`
отдаёт страницу и один общий итог. Суммы и даты обязаны считаться SQL-ем, а не
клиентом по страницам. Пример: поиск «стриж» нашёл «Машинку для стрижки волос»
(62,10 € Electronics) и завысил итог до 95,10 € вместо 33,00 €.

## What Changes

- `GET /api/reports/operations` принимает необязательный повторяемый `exclude_q`
  (до 10, каждый до 100 символов): операция, чей комментарий содержит ЛЮБОЙ из
  вариантов, не попадает ни в страницу, ни в итог. Считается фильтром.
- Необязательный `summary=true`: при хотя бы одном фильтре ответ содержит
  `summary: {first_date, last_date, by_category[{category_uuid, category_name, total_eq, count}]}`
  по ВСЕМ подходящим операциям периода.
- При `summary=true`, непустом `q` и `category_uuid` ответ содержит `other_matches` —
  такие же строки по категориям, которых нет в `category_uuid`, где совпал тот же `q`
  (с тем же `exclude_q`, `wallet_uuid`, `kind`, периодом).
- При `exclude_q` и `summary=true` блок `summary` присутствует всегда, даже при нуле
  операций: wallet-mcp по нему отличает новый wallet-report от старого.
- Без новых параметров ответ прежний.

## Capabilities

### New Capabilities
<!-- нет -->

### Modified Capabilities
- `operations-list`: новые требования «Исключение по тексту комментария», «Сводка по
  фильтру», «Совпадения вне фильтра категорий», «Проверка exclude_q».

## Impact

- **wallet-report**: `src/reports.cpp` (разбор параметров, JSON), `src/reports_repo.*`
  (`OperationsFilter.exclude_qs`, вызов `f_operations_by_category`), `openapi.yaml`,
  README.md (таблица эндпоинтов), `tests_integration/test_reports.cpp`.
- **wallet-api** (сопутствующий change `operations-search-summary-sql`): миграция 000042 —
  `p_exclude_qs` и `f_operations_by_category`.
- **wallet-mcp**: change `improve-search-summary` использует новые параметры.
- wallet-web, android-wallet, wallet-gateway — без изменений (новое необязательно, `/api/reports/*` прежний).
- Порядок выкатки: wallet-api → wallet-report → wallet-mcp.
