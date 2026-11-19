# Tasks

## 1. Тестовое зеркало синка

- [x] 1.1 В `src/sync_repo.cpp` перевести `currency_conversion` в `Scope::LedgerOnly` (форма таблицы из миграции 000043 wallet-api) и убедиться, что юнит-/интеграционная сборка проходит

## 2. Интеграционные тесты эквивалента

- [x] 2.1 `tests_integration/test_currency_routes.cpp`: эквивалент кошелька по маршруту `BTC → EUR → RUB` (`balances_at_dates`), сумма равна остатку × произведение курсов, блок не `incomplete`
- [x] 2.2 Кросс через USD (пары BTC/USD и USD/RUB) и приоритет: прямая пара > маршрут > USD
- [x] 2.3 Обрыв маршрута (нет курса у шага) — `balance_eq` пуст, блок `incomplete`, USD не подставляется; ни пары, ни маршрута, ни USD — тоже пусто
- [x] 2.4 `balance_series` по месяцам с эквивалентом по маршруту: по одной строке на кошелёк и бакет, без дублей
- [x] 2.5 Прогнать полный набор интеграционных тестов wallet-report на базе с миграцией 000043 и убедиться, что всё зелёное (существующие тесты без правок)

## Workflow follow-up

- Архивировать change после выката wallet-api и слияния; спека уйдёт в `openspec/specs/balance-reports/`.
- Порядок выката: wallet-api → wallet-report (рантайм не меняется) → wallet-web → android-wallet.
