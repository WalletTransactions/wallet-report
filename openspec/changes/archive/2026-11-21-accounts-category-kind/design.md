# Design

## Context

`list_accounts` (`src/reports_repo.cpp`) уже собирает строку справочника из `account` с
`LEFT JOIN` на `account_operating`/`account_asset`/`account_debt`/`currency`. Колонка
`account.category_kind` (0/1/2, по умолчанию 2) есть в схеме с wallet-api 000041. См. proposal.md.

## Decisions

**Правок в wallet-api и wallet-gateway не нужно.** Колонка и её разметка по истории операций
уже в миграции 000041; роль `wallet_reader` читает `account` целиком. Новых путей нет —
`/api/reports/accounts` остаётся под `/api/reports/*`.

**Поле отдаётся как число у всех строк.** Для кошельков и системных статей в БД уже 2 (сервер
этого не проверяет, решает клиент), поэтому отдельной ветки «у кошелька null», как у
`asset_type`, не делаем: клиенту проще одно правило.

**Обратная совместимость.** Поле добавляется в `required` ответа в `openapi.yaml`, но старые
потребители (wallet-telegram, wallet-mcp) лишнего поля не боятся; новый веб без поля
показывает все статьи.

## Risks / Trade-offs

- [Клиент принимает `category_kind` за ограничение] → в `openapi.yaml` и спеке явно сказано,
  что сервер сочетание вида и типа операции не проверяет.
