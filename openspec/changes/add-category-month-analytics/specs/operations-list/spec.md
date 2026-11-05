# Spec Delta

## ADDED Requirements

### Requirement: Исключение категорий из операций
`/operations` SHALL принимать необязательный `exclude_category_uuid` — список
uuid через запятую: операции, чья категория в списке, не попадают ни в
страницу, ни в итог по фильтру (`total_eq`, `total_count`). Прямой перевод
кошелёк → кошелёк категории не имеет и не исключается. Параметр считается
фильтром: с ним в ответе есть итог. Пустое значение равносильно отсутствию.

#### Scenario: Всё, кроме ремонта
- **WHEN** `kind=expense&exclude_category_uuid=<Ремонт>,<Мебель>`
- **THEN** в ответе и в `total_eq` все расходы периода, кроме операций «Ремонт» и «Мебель»

#### Scenario: Прямой перевод остаётся
- **WHEN** `exclude_category_uuid=<Ремонт>`, без `kind`, в периоде есть перевод «Наличные» → «Тинькофф»
- **THEN** перевод есть в ответе

#### Scenario: Исключение вместе с другими фильтрами
- **WHEN** `wallet_uuid=<Wirex>&exclude_category_uuid=<Alcohol>`
- **THEN** в ответе операции кошелька «Wirex», кроме операций «Alcohol»

### Requirement: Проверка фильтра-исключения
Сервис SHALL отвечать 400 до обращения к базе, если в `exclude_category_uuid`
есть не-uuid или пустой элемент либо больше 50 значений — с текстами
`exclude_category_uuid: некорректный uuid` и `exclude_category_uuid: не больше
50 значений`, а также если заданы оба — `category_uuid` и
`exclude_category_uuid` — с текстом `category_uuid и exclude_category_uuid
нельзя задавать вместе`.

#### Scenario: Оба списка
- **WHEN** `category_uuid=<Кафе>&exclude_category_uuid=<Бар>`
- **THEN** 400 `category_uuid и exclude_category_uuid нельзя задавать вместе`

#### Scenario: Битый uuid в исключении
- **WHEN** `exclude_category_uuid=abc`
- **THEN** 400 `exclude_category_uuid: некорректный uuid`
