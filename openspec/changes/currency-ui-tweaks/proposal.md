# Proposal

## Why

У валюты появляется флаг «показывать в эквиваленте» (`wallet-api:
currency-ui-tweaks`, колонка `currency.show_in_equivalent`). Веб берёт
справочник валют только из `GET /api/reports/currencies`. Пикеру
эквивалента нужен флаг для фильтрации. Новому списку валют с формой правки
(REST `/api/currencies*` в wallet-api) нужны флаг, пометка «общая/своя» и
текущие иконка и точность. Это часть набора мелких правок интерфейса
`currency-ui-tweaks`.

## What Changes

- Элемент `currencies[]` ответа `/api/reports/currencies` получает поля
  `show_in_equivalent` и `is_own` (оба boolean; `is_own: true` — своя
  валюта леджера), а также `icon_uuid` и `precision`.
- Состав и порядок списка не меняются: фильтрует клиент, потому что тот же
  справочник нужен целиком для пикеров валютных пар. Изменение
  обратно совместимо: только новые поля.

## Capabilities

### New Capabilities
<!-- нет -->

### Modified Capabilities
- `reference-data`: требование «Справочник валют» — в элементе списка
  появляются `show_in_equivalent`, `is_own`, `icon_uuid`, `precision`.

## Impact

- `src/reports_repo.cpp` (`list_currencies`: две колонки в обоих вариантах
  запроса, булевы значения в JSON), `src/reports_repo.hpp` (комментарий).
- `openapi.yaml` (`CurrenciesReport`), `README.md` (таблица эндпоинтов).
- `tests_integration/test_reports.cpp`.
- Зависит от миграции `wallet-api` 000026: без неё запрос падает на
  отсутствующей колонке, поэтому выкатывается после wallet-api.
