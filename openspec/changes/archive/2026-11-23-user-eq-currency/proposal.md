# Proposal

## Why

Issue [.github-private#19](https://github.com/WalletTransactions/.github-private/issues/19). Валюта эквивалента, которую пользователь
выбрал в вебе, теперь хранится на сервере (wallet-api `user-eq-currency`,
таблица `user_settings`). MCP ходит только в wallet-report (токен
`reports:read`), поэтому узнать выбор он может только отсюда. Веб тоже уже
грузит `/api/reports/currencies` и получает значение без лишнего запроса.

## What Changes

- `GET /api/reports/currencies` отдаёт рядом с `currencies` поле
  `preferred_eq_currency`: ISO-код из `user_settings` пользователя токена,
  если такая валюта есть в этом же списке, иначе `null`.
- Остальные поля и порядок не меняются.

## Capabilities

### New Capabilities

Нет.

### Modified Capabilities

- `reference-data`: требование «Справочник валют» дополняется полем
  `preferred_eq_currency`.

## Impact

- Код: `src/reports.cpp` (`handle_currencies`), репозиторий справочников,
  `openapi.yaml` (`CurrenciesReport`), тесты.
- Зависит от wallet-api `user-eq-currency` (таблица и `GRANT SELECT` для
  `wallet_reader`): выкатывать после неё.
- Пары: wallet-web и wallet-mcp `user-eq-currency` читают поле.
- Совместимость: новое поле в объекте ответа, клиенты, которые его не знают,
  игнорируют.
