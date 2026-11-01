# Spec Delta

## MODIFIED Requirements

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
