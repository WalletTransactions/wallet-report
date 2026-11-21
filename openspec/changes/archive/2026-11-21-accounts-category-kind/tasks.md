# Tasks

## 1. Поле category_kind в справочнике

- [x] 1.1 Добавить `a.category_kind` в запрос `list_accounts` и в JSON строки (`src/reports_repo.cpp`/`.hpp`); проверить: `GET /api/reports/accounts` на test отдаёт у статьи расхода 0, у статьи дохода 1, у кошелька 2
- [x] 1.2 Описать поле в `openapi.yaml` (`AccountRow`, `required`) и в таблице эндпоинтов `README.md`; проверить: выкаченный `/openapi.yaml` содержит `category_kind`
- [x] 1.3 Интеграционный тест в `tests_integration/test_reports.cpp`: умолчание 2 у кошелька и статьи; проверить: проект собирается и CI зелёный (PR #26); сам интеграционный тест на живой БД отдельно не гонялся — поведение подтверждено живым ответом test (1.1)

## Workflow follow-up

- Выкачено на test как 0.3.3-rc.2 (2026-11-21); change архивируется после выкатки.
