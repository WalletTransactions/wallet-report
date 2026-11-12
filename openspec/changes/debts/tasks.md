# Tasks

## 1. Зависимости и тестовые данные

- [ ] 1.1 Дождаться миграции wallet-api `debts` на test (таблицы, `v_account_operating.counterparty_uuid`, `f_debts_for_period`, `GRANT SELECT` для `wallet_reader`). Проверка: `SELECT * FROM f_debts_for_period(...)` ролью `wallet_reader` на test.
- [ ] 1.2 Тестовая копия `src/sync_repo.cpp`: `counterparty` и `account_debt` в `TableSpec` и в порядок применения (после `account_asset`), совпадающий с `kApplyOrder` wallet-api. Проверка: юнит-тест порядка и сид леджера с контрагентами и долговыми кошельками проходит в `tests_integration/`.

## 2. Балансовые отчёты

- [ ] 2.1 `/balance`, `/balance-at-dates`, `/balance-series`: `counterparty_uuid` на строке кошелька; `total_eq`/`incomplete` по кошелькам без актива и без долга; новые `receivable_eq`, `payable_eq`, `debts_incomplete`, `with_debts_eq` (design D1–D2). Проверка: интеграционные тесты на сценарии `specs/balance-reports` — выдача займа, получение займа, нет курса, серия без `accounts`.
- [ ] 2.2 Один общий предикат «ликвидный кошелёк» и один расчёт итогов долгов, используемые тремя отчётами. Проверка: тест, что итоги серии совпадают с суммами по счетам при `accounts=true`.

## 3. Операции, справочник, оборот

- [ ] 3.1 `/operations`: `kind = "debt"` и `counterparty_uuid` на строке (design D4). Проверка: интеграционные тесты на выдачу в долг, перевод между долговыми, прощение.
- [ ] 3.2 `/accounts` и `/wallet-turnover`: `LEFT JOIN account_debt`, поле `counterparty_uuid`. Проверка: интеграционные тесты `specs/reference-data`, `specs/flow-reports`; `/income` и `/expense` за период не включают суммы займа.

## 4. Отчёт «Долги»

- [ ] 4.1 `GET /api/reports/debts` (`reportDebts`) поверх `f_debts_for_period`, группировка по контрагенту, `receivable_eq`/`payable_eq`/`balance_eq`/`incomplete` (design D2–D3). Проверка: интеграционные тесты на сценарии `specs/debts-report` — две валюты у одного человека, два займа в одной валюте, переплата, нет курса, 400 без параметров.
- [ ] 4.2 `GET /api/reports/counterparties` (`reportCounterparties`) с `include_archived`. Проверка: интеграционный тест — архивный контрагент скрыт по умолчанию.

## 5. Контракт и документация

- [ ] 5.1 `openapi.yaml`: `reportDebts`, `reportCounterparties`, новые поля в ответах balance/operations/accounts/turnover, значение `debt` у `kind`; таблица эндпоинтов и описание итогов в `README.md`. Проверка: `openapi.yaml` проходит линтер, `GET /docs` отдаёт новые схемы.
- [ ] 5.2 Проверка выката на test: wallet-report поверх мигрированной базы. Проверка: `env-deploy`, интеграционные тесты wallet-report на test зелёные.

## Workflow follow-up

- Архивировать change после выката на prod и закрытия парных changes (`openspec archive`).
