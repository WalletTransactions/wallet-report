# Design

## Context

Мотивация — proposal.md, требования — specs/. Что важно для подхода:

- Все суммы отчётов считают SQL-функции wallet-api
  (`wallet-api/schema/function/f_*.sql`), wallet-report их только вызывает
  (`src/reports_repo.cpp`). Своих таблиц, функций и миграций у сервиса нет:
  роль `wallet_reader` без DDL.
- Итоги остатков (`total_eq`, `incomplete`) считаются **здесь**, оконными
  агрегатами поверх результата функции: `sum(balance_eq) OVER ()` в
  `balance_to_date`, `OVER (PARTITION BY date_index)` в `balances_at_dates` и
  `balance_series`, `GROUP BY date_index` в ветке «только итоги»
  `balance_series`.
- Классификация `kind` в ленте — внутри `f_operations_for_period` (`CASE` по
  `acc_from_is_operating`/`acc_to_is_operating`). Исключение ног перевода из
  дохода и расхода — внутри функций (`NOT EXISTS ... operation_link.type = 0`).
- `list_accounts` — единственный отчёт, который читает таблицы напрямую
  (`account`, `account_operating`, `currency`, `icon`), без функции.
- Интеграционные тесты сеют данные через тестовую копию протокола синка
  (`src/sync_repo.cpp`, не входит в бинарь сервиса), а схему берут из
  миграций wallet-api.

## Goals / Non-Goals

**Goals:**
- Отчёты различают ликвидные кошельки и активы, не меняя формулу баланса.
- Переоценка исключается из дохода, расхода и оборота в одном месте — в
  SQL-функциях wallet-api, — а не дублируется фильтрами в C++.
- Изменения ответов только добавляют поля; единственное смысловое изменение —
  `total_eq` остатков (ликвидные).

**Non-Goals:**
- Новая логика записи: активы и переоценку создают клиенты через синк.
- Фильтр `/operations` по типу (`add-operations-filters`) — отдельный change,
  см. Risks.
- Правки wallet-telegram и wallet-mcp: они продолжают читать `total_eq`.

## Decisions

### D1. Что требуется от wallet-api (парный change `capital-assets`)

| Объект | Правка | Кто читает здесь |
| --- | --- | --- |
| Таблица `account_asset` | новая; `GRANT SELECT ... TO wallet_reader` | `list_accounts`, `wallet_turnover_for_period` |
| `account.system_kind` | CHECK до `(0,1,2,3)` | тесты, `/accounts` |
| `f_balance_to_date`, `f_balance_to_dates`, `f_balance_series` | новые колонки `asset_type smallint` (NULL — ликвидный), `include_in_net_worth boolean` (у ликвидного `true`) | балансовые отчёты |
| `f_operations_for_period` | `kind = 'revaluation'`, если категорийная сторона проводки имеет `system_kind = 3`; категория/кошелёк заполняются как у дохода/расхода | `/operations` |
| `f_income_for_period`, `f_expanse_for_period`, `f_expanse_by_category_for_period`, `f_income_expense_by_month`, `f_expense_by_weekday_for_period` | исключить проводки, где одна сторона — категория с `system_kind = 3` | доход/расход/аналитика |
| `f_wallet_turnover_for_period` | исключить те же проводки | `/wallet-turnover` |
| `f_assets_for_period(ledger, from, to, eq_currency)` | новая, см. D4 | `/assets` |

Миграция функций меняет `RETURNS TABLE`, то есть `DROP` + `CREATE`, в одной
транзакции с грантами. wallet-gateway не меняется: новый путь под
`/api/reports/*`, скоуп `reports:read` выводится по префиксу (`src/access.cpp`).

### D2. Итоги остатков считает wallet-report, признак актива — функция

Раздельные итоги — те же оконные агрегаты с `FILTER`:
`sum(balance_eq) FILTER (WHERE asset_type IS NULL)` → `total_eq`,
`... FILTER (WHERE asset_type IS NOT NULL)` → `capital_eq`,
`... FILTER (WHERE asset_type IS NULL OR include_in_net_worth)` →
`net_worth_eq`; `bool_or(balance_eq IS NULL) FILTER (...)` → `incomplete` и
`capital_incomplete`. Так же и в ветке «только итоги» `balance_series`
(`GROUP BY date_index`).

*Альтернатива:* итоги в самих функциях wallet-api. Отклонена: сейчас итоги
живут здесь, и перенос затронул бы три функции ради одного потребителя;
android-wallet считает остатки сам.

*Альтернатива:* `LEFT JOIN account_asset` здесь, без изменения функций
балансов. Отклонена ради общего контракта четырёх changes: признак актива на
строке баланса — часть функции, как и валюта.

### D3. Оборот и справочник счетов читают `account_asset` напрямую

`wallet_turnover_for_period` добавляет `asset_type` через
`LEFT JOIN account_asset aa ON aa.ledger_id = $1 AND aa.uuid = account_uuid
AND aa.deleted_at IS NULL` поверх функции; `list_accounts` — тем же
`LEFT JOIN` рядом с `account_operating`. Функцию оборота меняем только ради
исключения переоценки, её сигнатура остаётся прежней.

*Альтернатива:* колонка `asset_type` в `f_wallet_turnover_for_period`.
Возможна; тогда JOIN здесь убирается. Выбор не меняет контракт ответа.

### D4. `/api/reports/assets` — тонкая обёртка над `f_assets_for_period`

Функция (wallet-api) по каждому живому активу отдаёт: `account_uuid`,
`account_name`, `asset_type`, `include_in_net_worth`, `is_archived`,
`currency_uuid`, `symbol`, `precision`, `invested`, `revaluation_total`,
`revaluation_period`, `value` и те же четыре суммы с суффиксом `_eq`, а также
`return_pct`. «На конец периода» — проводки с датой `< to + 1 день`, то есть
период включителен, как у `/income`. Итоги и `incomplete` — оконными
агрегатами здесь, как в D2. `return_pct` форматируется здесь (`format_pct`,
один знак), как проценты `/expense-structure`. Параметры и ошибки — как у
`/wallet-turnover`: `periodFrom`, `periodTo`, `eqCurrency`, `asUserId`.

### D5. Деградация до выкатки клиентов

Новые поля аддитивны, клиенты их игнорируют. `kind = "revaluation"` старый
веб покажет неизвестным типом — это закрывает парный change wallet-web.
Смысл `total_eq` меняется только при наличии активов, а их создаёт только
новый android-клиент.

## Risks / Trade-offs

- [Порядок выкатки] wallet-report с новыми колонками в запросах упадёт
  (500) на базе без миграции wallet-api → выкатка строго: миграция
  wallet-api, затем wallet-report. Обратный порядок безопасен: старый
  wallet-report выбирает колонки функций по имени и новые не видит.
- [Смысл `total_eq` у бота и MCP] команда остатков в wallet-telegram и
  инструменты wallet-mcp начнут показывать итог без активов → это решение
  issue («итог по умолчанию — ликвидные»); отдельной правки там не делаем.
- [Конфликт с `add-operations-filters`] его фильтр `kind` знает только
  `expense`/`income`/`transfer`, а сальдо без `kind` = доходы − расходы →
  какой change войдёт вторым, тот добавляет `revaluation` в допустимые
  значения фильтра и исключает её из сальдо.
- [Тестовая копия синка] `src/sync_repo.cpp` должна знать `account_asset`
  (`TableSpec` + порядок применения после `account_operating`) ровно как в
  wallet-api, иначе тесты не смогут сеять активы → переносим описание таблицы
  из wallet-api тем же коммитом, что и тесты.

## Migration Plan

1. wallet-api: миграция `capital-assets` (таблица, грант, CHECK, функции).
2. wallet-report: код, `openapi.yaml`, README; интеграционные тесты против
   базы с миграцией из п. 1.
3. Откат wallet-report — предыдущий образ (старые запросы совместимы с новой
   схемой). Откат миграции wallet-api — только после отката wallet-report.

## Open Questions

- Нужен ли в `/api/reports/assets` фильтр архивных активов на сервере, или
  клиент скрывает их сам по `is_archived` (сейчас — сам)? Ответ не меняет
  спеку: архивные возвращаются в любом случае.
