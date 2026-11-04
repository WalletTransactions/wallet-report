# Tasks

## 1. Миграция wallet-api

- [ ] 1.1 Сверить тела `f_expanse_by_category_for_period`, `f_operations_for_period`, `f_operations_total` с последней миграцией в `wallet-api/migrations/` (на 2026-11-04 — `000022` и `000027`; если `capital-assets` уже смержен — с его миграцией) и взять следующий свободный номер. Проверка: в описании PR wallet-api указаны номер и миграции, от которых взяты тела
- [ ] 1.2 Миграция: новая `f_expense_by_category_by_month` (design D1, D2) и `op_count` в `f_expanse_by_category_for_period` (D3) через `DROP FUNCTION` + `CREATE`, `GRANT EXECUTE … TO wallet_reader`; `down` возвращает прежние тела. Зеркала в `schema/function/` и `apply.sql`. Проверка: `migrate up` → `down` → `up` на чистой базе wallet-test без ошибок; на засеянном леджере `SELECT` из новой функции за год даёт по категориям те же итоги, что `f_expanse_by_category_for_period` за тот же период
- [ ] 1.3 В той же миграции `p_exclude_category_uuids uuid[] DEFAULT NULL` в конце сигнатур `f_operations_for_period` и `f_operations_total` (D4), прямой перевод не исключается. Проверка: вызов прежнего вида (без нового параметра, с `p_kind =>`) возвращает те же строки, что до миграции; с исключением категории её операций в результате нет, а прямой перевод остаётся

## 2. Число операций в структуре

- [ ] 2.1 `reports_repo`: читать `op_count` из `f_expanse_by_category_for_period`, отдавать в строке `/expense-structure`; у категории только из периода сравнения — `0`. Проверка: `TEST(expense_structure_op_count)` в `tests_integration/test_reports.cpp` — три расхода дают `op_count: 3`, категория только из cmp-периода — `0`, перевод с потерей — `op_count` числа переводов у Transfer (`tests_integration/run_local.sh`)
- [ ] 2.2 `openapi.yaml`: `op_count` в `ExpenseStructureRow` (обязательное, integer). Проверка: `swagger-cli validate openapi.yaml` — valid

## 3. Помесячный расход по категориям

- [ ] 3.1 `reports_repo::expense_by_category_by_month(ledger, from, to, eq)`: вызов новой функции, группировка строк в категории с месяцами, итоги категорий и общий `total_eq`/`incomplete` из SQL (D2), список всех месяцев периода от `date_trunc('month', from)` до `to`. Проверка: `TEST(expense_by_category_by_month_matches_structure)` — итоги категорий и общий итог совпадают с `/expense-structure` за тот же период, включая Transfer; `TEST(expense_by_category_by_month_months)` — пустой месяц есть в списке и отсутствует у категорий, у неполного первого месяца сумма только с `from`
- [ ] 3.2 `handle_expense_by_category_by_month` и маршрут `GET /api/reports/expense-by-category-by-month`: проверка `from`/`to`/`eq_currency` тем же кодом, что у `/expense-structure`, 400 до базы; без леджера — пустой ответ без запроса. Проверка: интеграционный тест на 400 без `eq_currency`; curl через гейтвей на wallet-test — ответ 200 с категориями и месяцами
- [ ] 3.3 `openapi.yaml`: операция `reportExpenseByCategoryByMonth` и схема ответа; в описании — что итог совпадает со `/expense-structure`, а не с `/income-expense-by-month` (ноги перевода погашены). `README.md`: строка в таблице эндпоинтов. Проверка: `swagger-cli validate openapi.yaml` — valid, `/openapi.yaml` с живого контейнера содержит операцию

## 4. Исключение категорий в операциях

- [ ] 4.1 `handle_operations`: разбор `exclude_category_uuid` тем же парсером, что `category_uuid`; 400 с текстами `exclude_category_uuid: некорректный uuid`, `exclude_category_uuid: не больше 50 значений`, `category_uuid и exclude_category_uuid нельзя задавать вместе` до базы; параметр — фильтр, с ним считается итог. Проверка: интеграционные тесты на каждый текст 400 и на «пустое значение = без фильтра»
- [ ] 4.2 `reports_repo`: `OperationsFilter.exclude_category_uuids`, именованный `p_exclude_category_uuids =>` в `append_filter_args`. Проверка: `TEST(operations_filter_exclude_categories)` — исключённых категорий нет ни в страницах, ни в `total_eq`/`total_count`; прямой перевод остаётся; вместе с `wallet_uuid` — по И; обход по курсору без пропусков
- [ ] 4.3 `openapi.yaml`: параметр `exclude_category_uuid` и новые тексты 400 у `reportOperations`; `README.md` — строка `/operations`. Проверка: `swagger-cli validate openapi.yaml` — valid

## 5. Нагрузка и живой стек

- [ ] 5.1 Сценарий k6: `/expense-by-category-by-month` за 36 месяцев на засеянном леджере loadtest. Проверка: прогон записан `record_run.sh`, p95 не более чем втрое выше `/expense-structure` за тот же период
- [ ] 5.2 Живой прогон на wallet-test после выкладки wallet-api и wallet-report: через гейтвей матрица за 2024-11..2026-10 даёт по «Alcohol» итог, равный `/expense-structure` за тот же период, и `op_count`, равный `total_count` у `/operations?category_uuid=<Alcohol>&kind=expense`. Проверка: curl-вывод приложен к тест-задаче

## Workflow follow-up

- Выкатка: сначала миграция wallet-api, затем wallet-report (design, Migration Plan).
- Потребитель — `wallet-mcp` change `add-category-analytics`; архивировать вместе с ним.
