# Tasks

## 1. Зависимость: wallet-api

- [x] 1.1 Change `operations-search-summary-sql` в wallet-api (миграция 000042) выполнен и выкачен на test. Проверка: на test-базе `SELECT * FROM f_operations_by_category(...)` выполняется ролью `wallet_reader`.

## 2. Разбор параметров

- [x] 2.1 `OperationsFilter.exclude_qs`, разбор `exclude_q` в `parse_operations_filter` с общей проверкой вариантов (тексты ошибок из спеки), `any()` учитывает `exclude_qs`. Проверка: интеграционный тест на 400 для 11 вариантов и для 101 символа, на `ё`/регистр.
- [x] 2.2 `append_filter_args` передаёт `p_exclude_qs => ARRAY[...]::text[]`. Проверка: интеграционный тест «Стрижка» + «Машинка для стрижки» — в ленте и в `total_eq` только «Стрижка».
- [ ] 2.3 Разбор `summary=true`. Проверка: `summary=false`/без параметра — ответ как раньше (HTTP-слой — проверяется на живом test, 4.2). **Не выполнено при архивации:** без summary в handler проверено только чтением кода, отдельного HTTP-теста нет.

## 3. Сводка и other_matches

- [x] 3.1 `operations_summary()` в `reports_repo`: вызов `f_operations_by_category`, `first_date`/`last_date`, строки `by_category` (NULL-категория, сортировка D3). Проверка: тест «две категории», согласованность суммы `by_category` и `total_eq`/`total_count`.
- [ ] 3.2 Замер времени `summary=true` на тестовой базе (год данных, `q` из 4 вариантов). Проверка: цифра в описании PR. **Не выполнено при архивации:** замер не делали.
- [x] 3.3 `other_matches` (D4): только при `summary` + `q` + `category_uuid`. Проверка: тесты «стрижка в другой категории», «нет совпадений вне фильтра → []», «без category_uuid → поля нет» (репозиторий покрыт тестами; условия показа other_matches в handler — проверяются на test, 4.2).
- [x] 3.4 `summary` при `exclude_q` и нуле операций (D5): `first_date/last_date: null`, `by_category: []`. Проверка: интеграционный тест.
- [ ] 3.5 Пользователь без леджера с `summary=true`: 200, пустой список, `total_eq: "0"`, `summary` пустой. Проверка: интеграционный тест. **Не выполнено при архивации:** отдельного теста пользователя без леджера нет, ветка покрыта только кодом.

## 4. Документация и выкатка

- [x] 4.1 `openapi.yaml` (`exclude_q`, `summary`, схемы `OperationsSummary`, `other_matches`, тексты 400) и README.md (таблица эндпоинтов). Проверка: Swagger UI на `/docs` показывает новые параметры.
- [x] 4.2 Выкатить на test после wallet-api (rc-тег). Проверка: `GET /api/reports/operations?...&q=стриж&exclude_q=машинк&summary=true` на test возвращает `summary` и `total_eq` 33.00 за 2026.
