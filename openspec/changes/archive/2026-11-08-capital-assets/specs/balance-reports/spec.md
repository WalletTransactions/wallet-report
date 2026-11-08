# Spec Delta

## MODIFIED Requirements

### Requirement: GET /api/reports/balance
Эндпоинт SHALL возвращать баланс каждого кошелька, у которого было движение,
с суммой в своей валюте и в эквиваленте, плюс итоги: `total_eq`/`incomplete`
— только по ликвидным кошелькам, `capital_eq`/`capital_incomplete` — по
активам, `net_worth_eq` — ликвидные плюс активы с `include_in_net_worth`.

#### Scenario: Нет параметра date
- **WHEN** `date` не передан
- **THEN** 400

#### Scenario: Ликвидный кошелёк и актив
- **WHEN** на дату у ликвидного кошелька 100 000, а у актива «Квартира» 12 000 000 (в эквиваленте)
- **THEN** в ответе обе строки, `total_eq` = 100 000, `capital_eq` = 12 000 000, `net_worth_eq` = 12 100 000

## ADDED Requirements

### Requirement: Признак актива на строке кошелька
Строка кошелька в `/balance`, `/balance-at-dates` и `/balance-series`
(`accounts=true`) SHALL содержать `asset_type` (1 — недвижимость, 2 — авто,
3 — прочее; `null` — ликвидный кошелёк) и `include_in_net_worth` (у
ликвидного кошелька — `true`). Актив — кошелёк с живой строкой
`account_asset`; порядок строк прежний.

#### Scenario: Актив в списке
- **WHEN** у кошелька «Машина» есть живая строка `account_asset` с `asset_type = 2`
- **THEN** его строка содержит `asset_type: 2`

#### Scenario: Признак актива снят
- **WHEN** строка `account_asset` кошелька мягко удалена
- **THEN** кошелёк возвращается как ликвидный (`asset_type: null`) и входит в `total_eq`

### Requirement: Раздельные итоги остатков
Каждый блок `/balance-at-dates` и каждый бакет `/balance-series` SHALL
содержать те же итоги, что `/balance`: `total_eq`/`incomplete` по ликвидным
кошелькам, `capital_eq`/`capital_incomplete` по активам и `net_worth_eq`. В
каждый итог входят только пересчитанные в эквивалент кошельки.

#### Scenario: Актив не входит в итог по умолчанию
- **WHEN** на дату есть только актив с балансом 5 000 000 в эквиваленте
- **THEN** `total_eq: "0"`, `capital_eq` = 5 000 000, `net_worth_eq` = 5 000 000

#### Scenario: Актив вне чистого капитала
- **WHEN** у актива `include_in_net_worth = false`
- **THEN** он входит в `capital_eq`, но не входит в `net_worth_eq`

#### Scenario: Нет курса у актива
- **WHEN** у актива нет курса к эквиваленту на дату, а у ликвидных кошельков есть
- **THEN** `incomplete: false`, `capital_incomplete: true`

#### Scenario: Только итоги в серии
- **WHEN** `/balance-series` вызван без `accounts`
- **THEN** каждый бакет содержит `total_eq`, `capital_eq`, `net_worth_eq` и оба флага, совпадающие с суммами по счетам при `accounts=true`
