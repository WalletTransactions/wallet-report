# Spec Delta

## ADDED Requirements

### Requirement: Признак долга на строке кошелька
Строка кошелька в `/balance`, `/balance-at-dates` и `/balance-series`
(`accounts=true`) SHALL содержать `counterparty_uuid` — `uuid` контрагента
долгового кошелька (кошелька с живой строкой `account_debt`); у остальных
кошельков `null`. Порядок строк прежний.

#### Scenario: Долговой кошелёк
- **WHEN** у кошелька «Вася · USD» есть живая строка `account_debt`
- **THEN** его строка содержит `counterparty_uuid` Васи

#### Scenario: Признак снят
- **WHEN** строка `account_debt` кошелька мягко удалена
- **THEN** кошелёк возвращается с `counterparty_uuid: null` и входит в `total_eq`

### Requirement: Долги вне ликвидных итогов
`total_eq` и `incomplete` SHALL считаться только по кошелькам, которые не
являются ни активом, ни долгом. Отдельно SHALL возвращаться `receivable_eq`
(сумма положительных остатков долговых кошельков), `payable_eq` (сумма
модулей отрицательных остатков), `debts_incomplete` и `with_debts_eq` =
`total_eq + receivable_eq − payable_eq`. Те же итоги SHALL быть в каждом
блоке `/balance-at-dates` и каждом бакете `/balance-series`. В итоги входят
только пересчитанные в эквивалент кошельки.

#### Scenario: Выдача займа переносит сумму из ликвидных в «мне должны»
- **WHEN** выдано в долг 100 с ликвидного кошелька
- **THEN** `total_eq` уменьшился на 100, `receivable_eq` вырос на 100, `with_debts_eq` не изменился

#### Scenario: Заём получен
- **WHEN** взято в долг 100, долговой кошелёк стал −100
- **THEN** `payable_eq` = 100, `total_eq` вырос на 100, `with_debts_eq` не изменился

#### Scenario: Нет курса у долга
- **WHEN** у долгового кошелька нет курса к эквиваленту на дату
- **THEN** `incomplete: false`, `debts_incomplete: true`

#### Scenario: Только итоги в серии
- **WHEN** `/balance-series` вызван без `accounts`
- **THEN** каждый бакет содержит `receivable_eq`, `payable_eq`, `with_debts_eq` и `debts_incomplete`, совпадающие с суммами по счетам при `accounts=true`
