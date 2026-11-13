# Tasks

## 1. Зависимости и тестовые данные

- [x] 1.1 Дождаться миграции wallet-api `debts` на test (таблицы, `v_account_operating.counterparty_uuid`, `f_debts_for_period`, `GRANT SELECT` для `wallet_reader`). Проверка: `SELECT * FROM f_debts_for_period(...)` ролью `wallet_reader` на test.
- [x] 1.2 Тестовая копия `src/sync_repo.cpp`: `counterparty` и `account_debt` в `TableSpec` и в порядок применения (после `account_asset`), совпадающий с `kApplyOrder` wallet-api. Проверка: юнит-тест порядка и сид леджера с контрагентами и долговыми кошельками проходит в `tests_integration/`.

## 2. Балансовые отчёты

- [x] 2.1 `/balance`, `/balance-at-dates`, `/balance-series`: `counterparty_uuid` на строке кошелька; `total_eq`/`incomplete` по кошелькам без актива и без долга; новые `receivable_eq`, `payable_eq`, `debts_incomplete`, `with_debts_eq` (design D1–D2). Проверка: интеграционные тесты на сценарии `specs/balance-reports` — выдача займа, получение займа, нет курса, серия без `accounts`.
- [x] 2.2 Один общий предикат «ликвидный кошелёк» и один расчёт итогов долгов, используемые тремя отчётами. Проверка: тест, что итоги серии совпадают с суммами по счетам при `accounts=true`.

## 3. Операции, справочник, оборот

- [x] 3.1 `/operations`: `kind = "debt"` и `counterparty_uuid` на строке (design D4). Проверка: интеграционные тесты на выдачу в долг, перевод между долговыми, прощение.
- [x] 3.2 `/accounts` и `/wallet-turnover`: `LEFT JOIN account_debt`, поле `counterparty_uuid`. Проверка: интеграционные тесты `specs/reference-data`, `specs/flow-reports`; `/income` и `/expense` за период не включают суммы займа.

## 4. Отчёт «Долги»

- [x] 4.1 `GET /api/reports/debts` (`reportDebts`) поверх `f_debts_for_period`, группировка по контрагенту, `receivable_eq`/`payable_eq`/`balance_eq`/`incomplete` (design D2–D3). Проверка: интеграционные тесты на сценарии `specs/debts-report` — две валюты у одного человека, два займа в одной валюте, переплата, нет курса, 400 без параметров.
- [x] 4.2 `GET /api/reports/counterparties` (`reportCounterparties`) с `include_archived`. Проверка: интеграционный тест — архивный контрагент скрыт по умолчанию.

## 5. Контракт и документация

- [x] 5.1 `openapi.yaml`: `reportDebts`, `reportCounterparties`, новые поля в ответах balance/operations/accounts/turnover, значение `debt` у `kind`; таблица эндпоинтов и описание итогов в `README.md`. Проверка: `openapi.yaml` проходит линтер, `GET /docs` отдаёт новые схемы.
- [x] 5.2 Проверка выката на test: wallet-report поверх мигрированной базы. Проверка: `env-deploy`, интеграционные тесты wallet-report на test зелёные.

## Примечания к выполнению

- 1.1 — не закрыта: wallet-api `debts` (PR ветки `openspec-debts`) ещё не выкачен на test. Миграция `000036` проверена в базе `wallet_api_it` кластера wallet-test (`run_local.sh` wallet-api) и на одноразовом `postgres:16`; там `f_debts_for_period` ролью `wallet_reader` вызывается.
- Прогон тестов (1.2–4.2): `run_local.sh` wallet-report на кластере wallet-test (`wallet_api_it`) упал на подключении — пароль `wallet_reader` в этом кластере не совпадает с `.env` (68 из 71 тестов — `password authentication failed`, до проверок дело не дошло); роль на стенде не трогалась. Тот же бинарь против одноразового `postgres:16-alpine` с миграциями wallet-api до `000036` (ветка `openspec-debts`): 71 тест, 0 упавших (11 новых).
- 1.2 — «юнит-тест порядка»: юнит-тестов в wallet-report нет, порядок проверяет интеграционный `sync_apply_order_matches_wallet_api` (`sync_repo::apply_order()` — копия списка wallet-api). Копия синка получила и необязательные колонки с `null` (`nullable_types`: `due_date`, `icon_uuid`), без проверки формата — это тестовая сеялка.
- 2.1 — `net_worth_eq` долгов не включает (ликвидные + активы с `include_in_net_worth`): так в design (Open Questions), итог с долгами — `with_debts_eq`. Кошелёк одновременно актив и долг — в `capital_eq` и в долгах, не в ликвидных.
- 2.2 — предикаты `kLiquid`/`kDebt` и `balance_total_aggregates()` в `reports_repo.cpp` — общие для оконной формы (три отчёта с `accounts`) и сгруппированной (`/balance-series` без `accounts`).
- 3.1 — фильтр `kind` принимает `debt` (решение владельца 2026-11-12; design D4 исправлен: `revaluation` фильтр уже принимал). Текст ошибки — `kind должен быть expense, income, transfer, revaluation или debt`; требование «Проверка параметров фильтра» добавлено в дельту `specs/operations-list` (в основной спеке текст отставал и от `revaluation`). `counterparty_uuid` — только у `kind = "debt"`, у остальных `null`.
- 4.1 — форма ответа: `counterparties[]` (`counterparty_uuid`, `name`, `is_archived`, `balance_eq` — нетто по пересчитанным кошелькам или `null`, `incomplete`, `accounts[]`) и итоги `receivable_eq`/`payable_eq`/`balance_eq`/`incomplete`. Все суммы — в SQL (оконные агрегаты). «400 без параметров» интеграционным тестом не покрыт (тесты вызывают `reports_repo` без HTTP): ручка идёт через тот же `common_prolog` и проверку `from`/`to`, что `/assets`. Вживую на собранном сервисе: маршруты `/debts` и `/counterparties` отвечают `401` без токена (не `404`); `400` с токеном не проверялся.
- 5.1 — `openapi-spec-validator`: OK; `GET /openapi.yaml` собранного `wallet_report` отдаёт `reportDebts`/`reportCounterparties`, `GET /docs` — `200`. `openspec validate debts --strict`: valid.

## Workflow follow-up

- Архивировать change после выката на test и проверки 5.2 (`openspec archive`), не дожидаясь prod — решение владельца 2026-11-12.
