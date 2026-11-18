# Design

## Context

- `/operations` (`src/reports.cpp::handle_operations`) собирает `OperationsFilter`
  (`parse_operations_filter`), зовёт `f_operations_for_period` и, если задан хоть один
  фильтр, вторым запросом `f_operations_total`. Параметры SQL-функций передаются
  именованной нотацией и только заданные (`append_filter_args`).
- Схема и SQL-функции живут в wallet-api; wallet-report читает роль `wallet_reader`.

## Goals / Non-Goals

**Goals:** `exclude_q`, `summary`, `other_matches` по контракту из спеки; ответ без новых параметров не меняется.

**Non-Goals:** группировка по месяцам или кошелькам; `other_matches` для `exclude_category_uuid`; новые пути (остаёмся под `/api/reports/*`).

## Decisions

### D1. Правка в wallet-api обязательна
Сопутствующий change `operations-search-summary-sql`: миграция 000042 добавляет
`p_exclude_qs` в `f_operations_for_period`/`f_operations_total` и функцию
`f_operations_by_category(...)` (категория → `matched`, `total_eq`, `first_date`,
`last_date`) с теми же фильтрами. Здесь — только вызов. Остальные сервисы
(wallet-gateway, wallet-web, android-wallet) не меняются.

### D2. Параметры
- `exclude_q`: разбор как у `q` (общая функция проверки вариантов; тексты ошибок
  `exclude_q: не больше 10 значений`, `exclude_q: не длиннее 100 символов`). В
  `OperationsFilter` — `std::vector<std::string> exclude_qs`, в SQL — `p_exclude_qs => ARRAY[...]::text[]`.
  Непустой `exclude_qs` входит в `any()` (с ним есть итог).
- `summary`: булев, `true` строго строкой `true`; любое другое значение — как отсутствие (как у остальных необязательных флагов эндпоинтов).

### D3. Сводка одним запросом
`summary.by_category` — строки `f_operations_by_category(<те же фильтры>)`;
`first_date`/`last_date` — `min`/`max` по этим строкам в коде (даты — значения из SQL,
не вычисления над суммами). Строка с `NULL` категорией отдаётся как `category_uuid: null`,
`category_name: ""` (клиент решает, показывать ли). Сортировка по убыванию `total_eq`,
затем по имени — для стабильного ответа.

### D4. other_matches
Второй вызов `f_operations_by_category` с теми же фильтрами, но `p_category_uuids` не
передаётся, а `p_exclude_category_uuids` = `category_uuid` запроса. Только при
`summary=true` + непустом `q` (или `qs`) + `category_uuid`: иначе вызова нет.

### D5. Признак поддержки для wallet-mcp
При `exclude_q` + `summary=true` блок `summary` присутствует всегда. Старый wallet-report
игнорирует неизвестный `exclude_q`, не вернёт `summary`, и wallet-mcp понимает, что
исключение не применилось, и отвечает «Отчёты временно недоступны», а не неверной суммой.

### D6. Ошибки
Порядок проверок в `parse_operations_filter`: после `q` — `exclude_q`. 400 до обращения к базе.

## Risks / Trade-offs

- **Лишние запросы.** `summary` — +1 запрос, `other_matches` — ещё +1; только по явному `summary=true` и фильтрам. Замер — tasks 3.2.
- **Рассогласование версий.** wallet-report с `summary` раньше миграции wallet-api упадёт 500 при `summary=true`/`exclude_q`; порядок выкатки (wallet-api первым) закреплён в tasks 4.1. Без новых параметров вызовы идут на старую сигнатуру.
- **Именование `NULL`-категории.** Прямые переводы и долги — единая строка; её не видно в разбивке по расходам, потому что с `kind=expense` они не подходят.
