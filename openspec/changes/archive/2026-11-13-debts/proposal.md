# Proposal

## Why

Часть набора `debts` (issue
[.github-private#10](https://github.com/WalletTransactions/.github-private/issues/10);
парные changes: `wallet-api` — схема, синк, SQL-функции; `android-wallet`;
`wallet-telegram`). Долг — это кошелёк контрагента с признаком `account_debt`:
баланс «+» значит «мне должны», «−» — «я должен». Сейчас отчёты не отличают
долговые кошельки от обычных: «выдал 10 000 Васе» уменьшило бы ликвидный
итог, а «Вася должен» нигде не видно отдельно.

## What Changes

- **Балансовые отчёты** (`/balance`, `/balance-at-dates`, `/balance-series`):
  на строке кошелька — `counterparty_uuid` (`null` — не долговой).
  **BREAKING (по смыслу):** `total_eq` и `incomplete` считаются только по
  кошелькам, не являющимся ни активом, ни долгом (решение issue: долги —
  отдельной строкой, а не ликвидные). Новые итоги: `receivable_eq` («мне
  должны», сумма положительных балансов долговых кошельков),
  `payable_eq` («я должен», сумма отрицательных, неотрицательным числом),
  `debts_incomplete` и `with_debts_eq` = `total_eq + receivable_eq −
  payable_eq`. Существующие поля не исчезают.
- **Лента операций** (`/operations`): `kind = "debt"` для перевода между
  долговым и недолговым кошельком (поверх `f_operations_for_period`
  wallet-api); у строки заполнены контрагент, кошелёк и `from`/`to`.
- **Оборот кошельков** (`/wallet-turnover`) и **справочник счетов**
  (`/accounts`): `counterparty_uuid` у кошелька — через `LEFT JOIN
  account_debt` в самом wallet-report.
- **Новый отчёт** `GET /api/reports/debts` (`reportDebts`) поверх
  `f_debts_for_period`: по каждому долговому кошельку и по каждому
  контрагенту — остаток, поступления и выбытия за период, срок; итоги в
  эквиваленте.
- **Контрагенты в справочнике**: `GET /api/reports/counterparties`
  (`reportCounterparties`) — живые контрагенты леджера для бота и веба.
- Тестовая копия синка (`src/sync_repo.cpp`) получает `counterparty` и
  `account_debt` в том же порядке, что `kApplyOrder` в wallet-api.

**Не входит:** запись (долги создают клиенты через синк wallet-api), проценты
и график платежей, включение долгов в `net_worth_eq` (см. Open Questions в
design).

## Capabilities

### New Capabilities
- `debts-report`: отчёт «Долги» — остатки по кошелькам и контрагентам,
  итоги «мне должны / я должен», справочник контрагентов.

### Modified Capabilities
- `balance-reports`: признак долга на строке, долги вне ликвидных итогов,
  новые итоги.
- `operations-list`: тип операции `debt`.
- `reference-data`: `counterparty_uuid` у кошелька в `/accounts`.
- `flow-reports`: `counterparty_uuid` в `/wallet-turnover`.

## Impact

- **Код:** `src/` обработчики balance/operations/accounts/turnover, новый
  `debts` и `counterparties`, тестовая копия `sync_repo.cpp`; `openapi.yaml`,
  таблица эндпоинтов в `README.md`.
- **wallet-api:** функции и колонки — парный change; здесь только вызов и
  `LEFT JOIN`; роль `wallet_reader` получает `SELECT` на новые таблицы там.
- **wallet-web:** потребитель; правок в этом change нет.
- **Выкат:** после wallet-api (миграция), до Android-версии с долгами.
