## 1. Эндпоинт

- [x] 1.1 `reports_repo::data_range` — один `SELECT min(date), max(date), count(*)` по живым операциям леджера; проверка: интеграционный тест на леджер с операциями, с удалённой операцией и на пустой леджер
- [x] 1.2 Хендлер `GET /api/reports/data-range` и регистрация маршрута; проверка: сборка проходит, `curl` без токена — 401

## 2. Контракт и документация

- [x] 2.1 Запись в `openapi.yaml` (`reportDataRange`, схема `DataRangeReport`) и строка в таблице эндпоинтов `README.md`; проверка: `openspec validate add-data-range-endpoint` проходит

## 3. Выкатка

- [ ] 3.1 Выкатить на test; проверка: `GET /api/reports/data-range` с токеном демо-аккаунта возвращает `first_date` `2025-12-01`, `operations_count` `475`
