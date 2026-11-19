# Design

## Context

Отчёты wallet-report не читают курс сами: все эквиваленты считают SQL-функции
wallet-api (`f_balance_to_date`, `f_balance_to_dates`, `f_balance_series`,
`f_assets_for_period`, `f_debts_for_period`, `v_operation_eq`), которые сервис
только вызывает ролью `wallet_reader`. В wallet-api (одноимённый change,
миграция 000043) курс стал функцией `rate_on(ledger, from, to, date)`:
прямой курс → явный маршрут → кросс через USD; потребители переведены на неё.
Изначальный замысел («составные строки во вьюхе») заменён по итогам замера —
см. design wallet-api, «Результат замера».

## Goals / Non-Goals

**Goals:**
- Подтвердить тестами на живой базе, что отчёты получают эквивалент по маршруту
  и через USD, сохраняют «нет курса — нет эквивалента» и не дублируют строки.
- Привести тестовое зеркало протокола синка к новой форме `currency_conversion`.

**Non-Goals:**
- Правки запросов отчётов, `openapi.yaml`, контракта ответов.
- Собственная логика курсов в wallet-report.

## Decisions

### 1. Правка wallet-api не нужна сверх одноимённого change
Нужны только миграция 000043 и `rate_on` из wallet-api; роль `wallet_reader`
получает `EXECUTE` на них миграцией (отдельного GRANT в wallet-report нет).
Wallet-gateway не затронут (пути `/api/reports/*` те же).

### 2. Тестовое зеркало синка
`src/sync_repo.cpp` — копия списка таблиц wallet-api для засева тестов, в
`wallet_report` не входит. `currency_conversion` переводится в `Scope::LedgerOnly`
(ключ конфликта `(ledger_id, uuid)` — под него в 000043 заведён уникальный
индекс). Построчные проверки (`num` 1…3 и т. п.) в зеркало не переносятся: тесты
засевают заведомо валидные шаги.

### 3. Тесты — отдельный файл
Сценарии маршрутов — в `tests_integration/test_currency_routes.cpp`, чтобы не
раздувать `test_reports.cpp`: засев через `sync_repo::push_batch`, проверки через
`reports_repo::balances_at_dates` и `balance_series`.

## Risks / Trade-offs

- [Кросс-конверсии дороже прямых (≈ 0,5–1 мс на строку)] → принято в wallet-api;
  отчёты ходят с периодом, строк мало. Узкое место — выборка по всему леджеру
  в валюте без прямых пар (см. замер wallet-api).
- [Тесты зависят от версии схемы тестовой базы] → `run_local.sh` накатывает
  миграции wallet-api перед прогоном; без 000043 новые тесты падают на засеве.

## Migration Plan

Порядок выката: wallet-api (000043) → wallet-report (только тесты и зеркало,
в рантайме изменений нет). Откат — откат wallet-api; wallet-report не меняется.
