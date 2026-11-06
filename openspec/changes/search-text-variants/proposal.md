# Proposal

## Why

Вопрос 2026-11-06 «когда я последний раз стригся?» модель решала четырьмя
вызовами `search_operations` подряд — «стриж», «парикмах», «barber»,
«haircut», — потому что `q` у `/api/reports/operations` — одна подстрока.
Итог по вариантам нельзя сложить из нескольких ответов: операция с
комментарием «стрижка у barber» попала бы в оба и посчиталась дважды.
Объединение должен считать SQL.

## What Changes

- `q` у `GET /api/reports/operations` можно повторить до 10 раз
  (`q=стриж&q=barber`): операция подходит, если её комментарий содержит
  ЛЮБОЙ из вариантов, те же правила (без регистра, `ё` = `е`, буквально).
  В итоге (`total_eq`, `total_count`) и в ленте она — один раз.
- Один `q` — как раньше, тем же вызовом SQL (`p_q`).
- Новая ошибка 400 `q: не больше 10 значений`; `q: не длиннее 100 символов`
  проверяется у каждого варианта.
- **wallet-api**: миграция `000032_operations_text_variants` — трейлинг-параметр
  `p_qs text[] DEFAULT NULL` у `f_operations_for_period` и `f_operations_total`
  (зеркала в `schema/function/`, `GRANT` в `schema/role/wallet_reader.sql`).

Потребитель — wallet-mcp, change `search-operations-variants-and-paging`
(`text` у `search_operations` — массив вариантов). Веб и бот шлют один `q`
и не меняются.

## Capabilities

### New Capabilities

Нет.

### Modified Capabilities

- `operations-list`: варианты `q` по ИЛИ и их проверка.

## Impact

- wallet-api: миграция 000032 (выкладывается раньше wallet-report; старый
  wallet-report после неё работает — `p_q` остаётся).
- wallet-report: `reports.cpp` (сбор повторённого `q`), `reports_repo`
  (`OperationsFilter::qs`, `p_qs => ARRAY[$n, …]::text[]`), `openapi.yaml`.
