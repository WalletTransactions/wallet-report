## 1. Поле в справочнике валют

- [ ] 1.1 `handle_currencies`: прочитать `user_settings.eq_currency_iso` пользователя токена (`sub`), отдать `preferred_eq_currency`, только если код есть в собранном списке, иначе `null`. Проверка: тесты по сценариям спеки (сохранено, не выбрано, нет у леджера, два участника); `ctest` зелёный
- [ ] 1.2 `openapi.yaml`: `preferred_eq_currency` (`string`, nullable) в `CurrenciesReport`; README. Проверка: линт openapi, как в CI

## 2. Выкат

- [ ] 2.1 rc на test после wallet-api `user-eq-currency`. Проверка: после `PUT /api/settings {"eq_currency":"EUR"}` ответ `/api/reports/currencies` с тем же пользователем несёт `preferred_eq_currency: "EUR"`
