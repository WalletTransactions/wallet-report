# Tasks

## 1. Выборка с фильтрами

- [x] 1.1 `reports_repo`: `OperationsFilter {category_uuids, wallet_uuids, kind, q}`, сборка вызова `f_operations_for_period` с позиционными `$1..$5` и именованными параметрами курсора и фильтров (design D1). Два нынешних текста запроса уходят. Проверка: существующие `operations_for_period_*` в `tests_integration/test_reports.cpp` проходят без правок поведения (`tests_integration/run_local.sh`)
  Одна сборка вызова; курсор оставлен позиционным `$6, $7` (фильтры — именованные после него), чтобы без фильтров текст был прежним и работал до миграции 000027 (design, Risks). `operations_for_period_*` зелёные без правок.
- [x] 1.2 Интеграционные тесты фильтров: каждый фильтр отдельно и вместе (И), список по ИЛИ, прямой перевод по любому из кошельков, uuid категории в `wallet_uuid` ничего не находит, uuid из чужого леджера ничего не находит, `q` без учёта регистра и с `ё` = `е`, `q` с `%`/`_` буквально, пустой комментарий не совпадает. Проверка: новые `TEST(operations_filter_*)` зелёные
  `operations_filter_each_filter_and_combinations`, `_wallet_side_and_cross_kind_uuids`, `_foreign_ledger_uuid_finds_nothing`, `_q_case_yo_and_literal_wildcards` — зелёные (`run_local.sh`, 41/41).
- [x] 1.3 Интеграционный тест пагинации по отфильтрованному набору: много операций, мало совпадений, маленький `limit` — обход по курсору с теми же фильтрами отдаёт каждую подходящую операцию ровно один раз, у последней страницы `has_more: false`. Проверка: `TEST(operations_filter_pages_without_gaps_or_duplicates)` зелёный
  3000 операций, 5 совпадений (две с одной датой), `limit=2`: страницы 2/2/1, `has_more: false`, без пропусков и дублей — зелёный.

## 2. Итог

- [x] 2.1 `reports_repo::operations_total(filter)` → `{total_eq, incomplete, matched}` через `f_operations_total` той же сборкой (D1, D3); `OperationsResult` получает поля итога. Проверка: интеграционный тест — итог за весь период при `limit` меньше числа совпадений; с `kind` — неотрицательная сумма; без `kind` — сальдо, прямые переводы дают 0, но входят в `matched`; операция без курса даёт `incomplete` и не входит в сумму
  `operations_total` + поля `has_total/total_eq/total_incomplete/total_count`; итог считается внутри `operations_for_period` только при фильтрах. `operations_total_sign_rule_incomplete_and_whole_period` зелёный.
- [x] 2.2 Тест согласованности: для нескольких наборов фильтров `matched` равен числу строк по всем страницам, а `total_eq` — их сумме с правилом знака, посчитанной в тесте. Проверка: `TEST(operations_total_matches_all_pages)` зелёный
  7 наборов фильтров, обход по 2 строки — `matched`, `total_eq`, `incomplete` совпадают; зелёный.

## 3. HTTP-ручка

- [x] 3.1 `handle_operations`: разбор и проверка `category_uuid`, `wallet_uuid`, `kind`, `q` с дословными текстами 400 (spec «Проверка параметров фильтра», design D2), порядок проверок после курсора. Второй запрос итога только при фильтрах, поля `total_eq`/`total_incomplete`/`total_count` в ответе; нет леджера — `"0"`/`false`/`0` без запроса. Проверка: curl через гейтвей на живом стеке — каждый текст 400 (битый uuid, пустой элемент, 51 значение, неверный `kind`, `q` из 101 кириллической буквы), 100 букв принимаются; ответ без фильтров совпадает побайтно с ответом до изменения на том же леджере
  Проверено на test (wallet-report 0.1.1-rc.1, wallet-api 0.1.1-rc.1), curl через gateway `:8091` на засеянном леджере loadtest: все пять текстов 400 дословно (битый uuid, пустой элемент, 51 значение, `kind=Expense`, `q` из 101 кириллической буквы); 100 букв и 50 значений приняты; `category_uuid`, `wallet_uuid`+`kind`, `q` без учёта регистра, `q` с `%` буквально, чужой uuid → пусто, итог только при фильтрах, пустые параметры = без фильтров. Побайтно: старый образ 0.1.0-rc.2 временно поднят в сети `wallet-test` с окружением текущего — 7 запросов без фильтров (разные периоды, `limit`, курсор, `eq_currency`, пустой период) совпали с 0.1.1-rc.1 байт в байт.
- [ ] 3.2 `openapi.yaml`: параметры и 400 у `reportOperations`, необязательные `total_eq`/`total_incomplete`/`total_count` в `OperationsReport` с правилом знака, новое описание ручки. `README.md`: строка `/operations` в таблице эндпоинтов. Комментарии в `reports.cpp`/`reports_repo.hpp` про «фильтров на сервере нет» обновлены. Проверка: `/docs` на живом стеке показывает новые параметры, YAML проходит линтер Swagger UI без ошибок
  Сделано: openapi (4 параметра, 400, поля итога с правилом знака, описание), README, комментарии; YAML разбирается `yaml.safe_load`. `/docs` на живом стеке: контейнер wallet-report test отдаёт `/docs` (200) и `/openapi.yaml`, побайтно равный main, — в `reportOperations` параметры `category_uuid`/`wallet_uuid`/`kind`/`q`, ответ 400, поля `total_*`. Через gateway `/docs` — документация самого gateway, не wallet-report. Открыто — линтер Swagger UI (на машине нет валидатора, скачивание из npm не разрешено).

## 4. Нагрузка

- [x] 4.1 Сценарий в `loadtest/k6`: лента за год с `q` и с `category_uuid` на засеянных данных. Проверка: прогон записан `record_run.sh`, p95 с фильтрами не более чем вдвое выше ленты без фильтров того же периода
  Прогон `record_run.sh` (BRANCH `add-operations-filters`, VERSION 0.1.1-rc.1, 10 и 30 VU по 60 с) записан в perf_history, 0% ошибок. p95: 10 VU — без фильтров 55.2 мс, `q` 51.0, `category_uuid` 59.3 (×1.07); 30 VU — 225.0 / 226.7 / 235.2 (×1.05). Критерий «не более чем вдвое» выполнен.

## Workflow follow-up

- Выкатывается после миграции wallet-api `add-operations-filter-functions`. Архивировать оба change вместе.
- Потребитель — `wallet-mcp` (`search_operations`). Переход веба и бота на серверные фильтры — отдельные changes по желанию.
