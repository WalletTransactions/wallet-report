# Proposal

## Why

Это часть отчётов набора `capital-assets` (issue
[.github-private#1](https://github.com/WalletTransactions/.github-private/issues/1),
вариант A3; парные changes: `wallet-api` — схема, синк и SQL-функции,
`android-wallet`, `wallet-web`). Покупка квартиры, ремонт как вложение и
машина должны перестать быть операционным расходом: деньги переходят в
актив — обычный кошелёк с признаком `account_asset`, — а изменение его оценки
проводится переоценкой через системную категорию «Переоценка»
(`system_kind = 3`). Сейчас отчёты не различают ликвидные кошельки и активы:
итог остатков смешивает их, а проводка переоценки выглядела бы обычным
доходом или расходом.

## What Changes

- **Балансовые отчёты** (`/balance`, `/balance-at-dates`, `/balance-series`):
  на строке кошелька — `asset_type` (`null` — ликвидный кошелёк) и
  `include_in_net_worth`. **BREAKING (по смыслу):** `total_eq` и `incomplete`
  считаются только по ликвидным кошелькам (решение issue: «итог по умолчанию —
  ликвидные»). Новые итоги `capital_eq` / `capital_incomplete` (все активы) и
  `net_worth_eq` (ликвидные + активы с `include_in_net_worth`). Форма ответа
  расширяется, существующие поля не исчезают.
- **Лента операций** (`/operations`): новый `kind = "revaluation"` для проводок
  между активом и категорией «Переоценка»; у такой строки заполнены категория
  «Переоценка», кошелёк-актив и `from`/`to`, по которым видно направление.
- **Доход, расход и аналитика** (`/income`, `/expense`, `/expense-structure`,
  `/expense-biggest-moves`, `/income-expense-by-month`, `/expense-by-weekday`):
  переоценка не входит ни в доход, ни в расход. Считается в SQL-функциях
  wallet-api, здесь меняются только спеки и тесты.
- **Оборот кошельков** (`/wallet-turnover`): переоценка не входит в оборот;
  на строке — `asset_type`, чтобы клиент сгруппировал активы отдельно.
- **Справочник счетов** (`/accounts`): `asset_type` и `include_in_net_worth`
  у кошелька; системная категория «Переоценка» (`system_kind = 3`)
  возвращается, как остальные системные.
- **Новый отчёт** `GET /api/reports/assets` (`reportAssets`) поверх
  `f_assets_for_period` wallet-api: по каждому активу — вложено, переоценка
  накопленная и за период, оценка на конец периода, доходность; итоги в
  эквиваленте.

Вне scope: запись (переоценку и активы создают клиенты через синк wallet-api),
обязательства и ипотека, бот / wallet-ai / импорт выписок, механизм
минимальной версии клиента.

## Capabilities

### New Capabilities
- `capital-report`: отчёт по капитальным активам — вложено, переоценка,
  оценка и доходность по каждому активу и итогом.

### Modified Capabilities
- `balance-reports`: признак актива на строке, раздельные итоги
  (ликвидные / капитал / всего с капиталом).
- `operations-list`: тип операции `revaluation`.
- `flow-reports`: переоценка вне дохода, расхода и оборота кошельков;
  `asset_type` в обороте.
- `expense-analytics`: переоценка не входит в структуру расхода и топ
  изменений.
- `reference-data`: `asset_type` / `include_in_net_worth` в справочнике
  счетов, системная категория «Переоценка».

## Impact

- `src/reports_repo.cpp`/`.hpp`, `src/reports.cpp` — новые колонки, итоги,
  эндпоинт `/api/reports/assets`.
- `openapi.yaml`, `README.md` (таблица эндпоинтов).
- `tests_integration/test_reports.cpp`; тестовая копия `src/sync_repo.cpp`
  получает `account_asset` в `TableSpec` (как в wallet-api).
- Зависимость от `wallet-api` (`capital-assets`): таблица `account_asset`,
  грант `wallet_reader`, `system_kind = 3`, новые колонки и правила в
  `f_balance_to_date(s)`, `f_balance_series`, `f_operations_for_period`,
  доход/расход-функциях и `f_wallet_turnover_for_period`, новая
  `f_assets_for_period`. Выкатка: сначала миграция wallet-api, потом
  wallet-report.
- Потребители смысла `total_eq`: wallet-web (парный change), wallet-telegram
  (`/balance` в команде остатков) и wallet-mcp — после выкатки они
  показывают итог ликвидных кошельков. Гейтвей не меняется (`/api/reports/*`).
