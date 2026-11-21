# Proposal

## Why

Форма ручного ввода операции в wallet-web («+ Операция», wallet-web#49) должна для «Расхода»
предлагать статьи расхода, а для «Дохода» — статьи дохода. Вид статьи хранится в
`account.category_kind` (wallet-api, миграция 000041: 0 — расход, 1 — доход, 2 — оба), но
`GET /api/reports/accounts` его не отдавал, и клиенту приходилось показывать все статьи.

## What Changes

- В каждую строку ответа `GET /api/reports/accounts` добавляется целое поле `category_kind`
  (0 — расход, 1 — доход, 2 — оба; у кошельков и системных статей всегда 2).
- Поле необязательно для старых клиентов: они его игнорируют, форма wallet-web без него
  показывает все статьи.
- Сервер сочетание вида статьи и типа операции не проверяет — это подсказка клиенту.

## Capabilities

### New Capabilities
Нет.

### Modified Capabilities
- `reference-data`: строка справочника кошельков и категорий получает `category_kind`.

## Impact

- **wallet-report**: `src/reports_repo.cpp`/`.hpp` (колонка `a.category_kind` в `list_accounts`),
  `openapi.yaml` (`AccountRow`), `README.md` (строка таблицы эндпоинтов),
  `tests_integration/test_reports.cpp`.
- **wallet-api / wallet-gateway**: правок нет, колонка уже есть в схеме.
- **Потребитель**: wallet-web (форма «+ Операция», change `add-manual-operation`).
