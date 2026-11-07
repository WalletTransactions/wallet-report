# Tasks

Все интеграционные тесты идут против базы с миграцией wallet-api
`capital-assets` (design D1): `tests_integration/run_local.sh` накатывает
миграции wallet-api.

## 1. Тестовая инфраструктура

- [x] 1.1 Тестовая копия синка `src/sync_repo.cpp`: `account_asset` в `TableSpec` (`asset_type`, `include_in_net_worth`, `comment`) и в порядке применения сразу после `account_operating` — ровно как в wallet-api. Хелперы `account_asset_row(...)` и `account_row` с параметром `system_kind` в `tests_integration/test_reports.cpp`. Проверка: все существующие `TEST(...)` в `test_reports.cpp` зелёные, плюс новый `TEST(seed_asset_and_revaluation_category_via_push)` сеет актив и категорию `system_kind = 3` одним батчем без конфликтов

## 2. Балансовые отчёты

- [x] 2.1 `reports_repo`: `balance_to_date`, `balances_at_dates`, `balance_series` (обе ветки) выбирают `asset_type`/`include_in_net_worth` из функций и считают `total_eq`/`incomplete`, `capital_eq`/`capital_incomplete`, `net_worth_eq` агрегатами с `FILTER` (design D2); строки получают `asset_type` числом или `null`, `include_in_net_worth` настоящим bool. Проверка: новые интеграционные тесты — ликвидный кошелёк + актив (сценарий spec «Ликвидный кошелёк и актив»), актив с `include_in_net_worth = false`, актив без курса (`capital_incomplete` без `incomplete`), снятый признак актива; `balance_series_totals_only_matches_per_account_totals` расширен на три итога
- [ ] 2.2 `reports.cpp` / `reports_repo.hpp`: новые поля в JSON трёх ответов. `openapi.yaml`: `BalanceReportRow` (`asset_type`, `include_in_net_worth`), `BalanceReport`, `BalanceAtDatesBlock`, `BalanceSeriesBucket` (`capital_eq`, `capital_incomplete`, `net_worth_eq`, новый смысл `total_eq`/`incomplete`); `README.md` — строка `/balance` в таблице эндпоинтов. Проверка: `/docs` на живом стеке показывает поля, curl `/api/reports/balance` через гейтвей возвращает три итога

## 3. Лента, доход, расход, оборот

- [x] 3.1 Интеграционный тест ленты: переоценка вверх и вниз получают `kind: "revaluation"` с категорией «Переоценка», кошельком-активом и правильными `from_uuid`/`to_uuid`; перевод в актив остаётся `transfer`. `openapi.yaml`: `revaluation` в enum `OperationRow.kind` и описание направления. Проверка: `TEST(operations_for_period_classifies_revaluation)` зелёный
- [x] 3.2 Интеграционные тесты исключения переоценки: `/income`, `/expense`, `income_expense_by_month`, `expense_by_weekday`, `expense_structure_for_period` (доля «Продуктов» `"100.0"` при износе машины), `expense_biggest_moves` (переоценка в одном периоде не попадает в топ). Проверка: новые `TEST(*_excludes_revaluation)` зелёные
- [x] 3.3 `wallet_turnover_for_period`: `asset_type` через `LEFT JOIN account_asset` (design D3) и JSON-поле числом или `null`; интеграционный тест — перевод в актив входит в `turnover_in`, переоценка не входит, у строки `asset_type: 1`. `openapi.yaml`: `WalletTurnoverRow.asset_type`. Проверка: `TEST(wallet_turnover_excludes_revaluation_and_marks_asset)` зелёный

## 4. Справочник счетов

- [x] 4.1 `list_accounts`: `LEFT JOIN account_asset` (живая строка), поля `asset_type`/`include_in_net_worth` (у категории `null`). Интеграционный тест — актив среди кошельков с `asset_type`, категория «Переоценка» с `system_kind: 3`, у категории оба поля `null`. `openapi.yaml`: `AccountRow`, `system_kind` с значением 3. Проверка: расширенный `TEST(list_accounts_splits_by_account_operating_and_filters_archived)` и новый `TEST(list_accounts_marks_assets)` зелёные

## 5. Отчёт «Капитал»

- [x] 5.1 `reports_repo::assets_for_period` поверх `f_assets_for_period`: строки по каждому активу, итоги `invested_eq`/`revaluation_total_eq`/`revaluation_period_eq`/`value_eq` и `incomplete` оконными агрегатами, `return_pct` через `format_pct` (design D4). Проверка: интеграционные тесты по сценариям spec `capital-report` — покупка + ремонт, продажа, переоценка до и внутри периода, актив без движения, ничего не вложено (`return_pct: null`), нет курса (`incomplete`), обычный кошелёк не попадает, порядок по `value_eq` убыв.
- [ ] 5.2 `GET /api/reports/assets` в `reports.cpp`: параметры и ошибки как у `/wallet-turnover`, нет леджера — `200` и пустой список. `openapi.yaml`: путь с `operationId: reportAssets`, схемы `AssetsReport`/`AssetRow`; `README.md` — строка в таблице эндпоинтов. Проверка: curl через гейтвей на живом стеке — `200` с данными, `400` без `from`/`eq_currency`, `401` без сессии; `/docs` показывает ручку

## 6. Интеграционная проверка

- [ ] 6.1 На стеке с миграцией wallet-api и засеянным активом (перевод, ремонт, переоценка вверх, износ) сверить вручную: `capital_eq` из `/balance` на дату после конца периода равен итоговому `value_eq` из `/assets`; `/income` и `/expense` за период совпадают с ответами до засева переоценки. Проверка: результаты сверки записаны в PR

Статус реализации (2026-11-07): код, `openapi.yaml` (redocly lint чистый) и README для 2.2 и 5.2 готовы, 61/61 интеграционных тестов зелёные на базе с миграцией wallet-api `000033`; открыты только живые проверки 2.2, 5.2, 6.1 — после выката на test. Сверх задач: `kind=revaluation` принимается фильтром `/operations` (design, Risks — `add-operations-filters` уже влит).

## Workflow follow-up

- Выкатывается после миграции wallet-api `capital-assets` (design, Migration Plan).
- С `add-operations-filters`: change, который войдёт вторым, добавляет `revaluation` в фильтр `kind` и исключает её из сальдо (design, Risks).
- Архивировать вместе с парными changes wallet-api, android-wallet, wallet-web.
