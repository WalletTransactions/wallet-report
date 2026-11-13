# reference-data Specification

## Purpose
Справочники, видимые леджеру, без привязки к периоду: валюты (для выбора
эквивалента), кошельки и категории (для Telegram-бота), иконки. Контракт —
`openapi.yaml` (`reportCurrencies`, `reportAccounts`, `reportIcons`).

## Requirements

### Requirement: Справочник валют
`/currencies` SHALL возвращать общие валюты (`ledger_id IS NULL`) плюс
собственные валюты леджера, отсортированные по `iso_code`. При совпадении
`iso_code` в списке только собственная валюта. Параметры `eq_currency`,
`from`/`to` не требуются. Каждый элемент SHALL нести, кроме прежних полей,
булевы `show_in_equivalent` и `is_own` (`true` — валюта леджера), `icon_uuid`
и целое `precision`. Валюты со снятым флагом из списка не убираются.

#### Scenario: Своя валюта леджера
- **WHEN** у леджера есть собственная валюта
- **THEN** она присутствует в списке с `is_own: true`

#### Scenario: Нет леджера
- **WHEN** у пользователя нет леджера
- **THEN** 200 и только общий справочник, у всех элементов `is_own: false`

#### Scenario: Валюта скрыта из эквивалента
- **WHEN** у своей валюты `BTC` снят флаг `show_in_equivalent`
- **THEN** `BTC` остаётся в списке с `show_in_equivalent: false`

#### Scenario: Поля для формы правки
- **WHEN** у своей валюты `KZT` точность 2 и иконка-флаг `X`
- **THEN** элемент `KZT` несёт `precision: 2` (число) и `icon_uuid` = uuid иконки `X`

#### Scenario: Своя валюта перекрывает общую
- **WHEN** у леджера своя `USD` с `show_in_equivalent: false`, а общая `USD` — с `true`
- **THEN** в списке одна `USD` — своя: `is_own: true`, `show_in_equivalent: false`

### Requirement: Справочник кошельков и категорий
`/accounts` SHALL возвращать живые (не удалённые) счета леджера: сначала
кошельки (счёт с живой строкой `account_operating`), затем категории, внутри —
по имени. Системные категории не скрываются (`system_kind` 1/2/3, где 3 —
«Переоценка»). Архивные счета по умолчанию исключены. У кошелька есть
`asset_type` (`null` — ликвидный) и `include_in_net_worth`; у категории оба
поля `null`.

#### Scenario: Фильтр по виду
- **WHEN** передан `kind=wallet` или `kind=category`
- **THEN** возвращаются только счета этого вида

#### Scenario: Архивные счета
- **WHEN** `include_archived=true`
- **THEN** архивные счета включаются в ответ

#### Scenario: Некорректные параметры
- **WHEN** `kind` не из `{wallet, category}` или `include_archived` не `true`/`false`
- **THEN** 400

#### Scenario: Нет леджера
- **WHEN** у пользователя нет леджера
- **THEN** 200 и `accounts: []`

#### Scenario: Актив среди кошельков
- **WHEN** у кошелька «Квартира» есть живая строка `account_asset` с `asset_type = 1`
- **THEN** он возвращается среди кошельков (`kind: "wallet"`) с `asset_type: 1`

#### Scenario: Системная категория «Переоценка»
- **WHEN** в леджере есть категория с `system_kind = 3`
- **THEN** она возвращается среди категорий с `system_kind: 3`

### Requirement: Иконки леджера
`/icons` SHALL возвращать `common` — общий набор (`icon.ledger_id IS NULL`) и
`user` — собственные иконки леджера, отсортированные по имени. Содержимое
изображения не отдаётся: клиент получает его по `sha256` из wallet-api.

#### Scenario: Нет леджера
- **WHEN** у пользователя нет леджера
- **THEN** 200, `common` заполнен, `user: []`

### Requirement: Признак долга в справочнике кошельков
Кошелёк в `/accounts` SHALL содержать `counterparty_uuid` (`null` — не
долговой), определяемый по живой строке `account_debt`. Остальные поля и
порядок строк не меняются.

#### Scenario: Долговой кошелёк в справочнике
- **WHEN** у кошелька есть живая строка `account_debt`
- **THEN** его запись содержит `counterparty_uuid` контрагента

#### Scenario: Бот отбирает кошельки для трат
- **WHEN** wallet-telegram запрашивает `/accounts`
- **THEN** по `counterparty_uuid != null` он может исключить долговые кошельки
