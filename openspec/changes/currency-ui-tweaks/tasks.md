# Tasks

## 1. Справочник валют

- [ ] 1.1 `src/reports_repo.cpp` `list_currencies`: добавить `c.show_in_equivalent`, `(c.ledger_id IS NOT NULL)`, `c.icon_uuid`, `c."precision"` в оба запроса и в `kColumns`; булевы — JSON-boolean, `precision` — число; проверка — сборка и ручной `curl /api/reports/currencies` через gateway показывает у общей валюты `"show_in_equivalent": true, "is_own": false`, `icon_uuid` и числовой `precision`
- [ ] 1.2 Интеграционные тесты в `tests_integration/test_reports.cpp` по сценариям `reference-data` «Справочник валют» (своя → `is_own: true`; без леджера → все `is_own: false`; снятый флаг не убирает валюту; поля для формы правки; своя перекрывает общую с её флагом); проверка — `tests_integration/run_local.sh` зелёный на БД с миграцией wallet-api 000026

## 2. Контракт и документация

- [ ] 2.1 `openapi.yaml`: в элемент `CurrenciesReport.currencies` добавить обязательные `show_in_equivalent: boolean`, `is_own: boolean`, `icon_uuid: string (uuid)`, `precision: integer` с описанием; проверка — поля видны в `reportCurrencies`
- [ ] 2.2 `README.md`: строка `/api/reports/currencies` в таблице эндпоинтов упоминает новые поля; комментарий над `list_currencies` в `src/reports_repo.hpp` обновлён; проверка — просмотр диффа

## Workflow follow-up

- Выкатка строго после wallet-api (миграция 000026).
- Архивировать вместе с парными `currency-ui-tweaks`.
