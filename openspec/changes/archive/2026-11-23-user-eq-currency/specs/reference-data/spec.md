## MODIFIED Requirements

### Requirement: Справочник валют
`/currencies` SHALL возвращать общие валюты (`ledger_id IS NULL`) плюс
собственные валюты леджера, отсортированные по `iso_code`. При совпадении
`iso_code` в списке только собственная валюта. Параметры `eq_currency`,
`from`/`to` не требуются. Каждый элемент SHALL нести, кроме прежних полей,
булевы `show_in_equivalent` и `is_own` (`true` — валюта леджера), `icon_uuid`
и целое `precision`. Валюты со снятым флагом из списка не убираются. Рядом с
`currencies` ответ SHALL нести `preferred_eq_currency` — сохранённую
пользователем токена валюту эквивалента (wallet-api `user_settings`), если
она есть в этом списке, иначе `null`.

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

#### Scenario: Сохранённая валюта эквивалента
- **WHEN** пользователь сохранил `EUR`, и `EUR` есть в справочнике
- **THEN** ответ несёт `preferred_eq_currency: "EUR"`

#### Scenario: Валюта не выбрана или её нет у леджера
- **WHEN** пользователь ничего не сохранял, или сохранил `USD`, а `USD` в списке нет
- **THEN** `preferred_eq_currency: null`

#### Scenario: Общий леджер
- **WHEN** два участника одного леджера сохранили `EUR` и `RUB`
- **THEN** каждый получает в `preferred_eq_currency` своё значение
