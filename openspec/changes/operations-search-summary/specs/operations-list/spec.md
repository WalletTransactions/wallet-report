## ADDED Requirements

### Requirement: Исключение по тексту комментария
`/operations` SHALL принимать параметр `exclude_q`, повторённый до 10 раз:
операция, чей комментарий содержит ЛЮБОЙ из вариантов (без учёта регистра, `ё` =
`е`, буквально), не попадает ни в страницу, ни в итог по фильтру (`total_eq`,
`total_count`), ни в `summary`. Пустые значения отбрасываются. Операция с пустым
комментарием не исключается. Параметр считается фильтром: с ним в ответе есть
итог. Фильтры объединяются по И, в том числе с `q`.

#### Scenario: Ложное срабатывание
- **WHEN** `q=стриж&exclude_q=машинк`, комментарии «Стрижка» и «Машинка для стрижки волос»
- **THEN** в ответе и в `total_eq` только «Стрижка»

#### Scenario: Только исключение
- **WHEN** `exclude_q=перевод`, без других фильтров
- **THEN** в ответе все операции периода, кроме тех, где в комментарии есть «перевод», и есть `total_eq`

#### Scenario: Пустой комментарий
- **WHEN** `exclude_q=такси`, у операции комментарий пуст
- **THEN** операция есть в ответе

### Requirement: Проверка exclude_q
Сервис SHALL отвечать 400 до обращения к базе, если вариантов `exclude_q`
(непустых) больше 10 — с текстом `exclude_q: не больше 10 значений`, или если любой
вариант длиннее 100 символов либо не UTF-8 — с текстом `exclude_q: не длиннее 100
символов`.

#### Scenario: Одиннадцать вариантов
- **WHEN** `exclude_q` повторён 11 раз
- **THEN** 400 `exclude_q: не больше 10 значений`

#### Scenario: Длинный вариант
- **WHEN** `exclude_q` — 101 кириллическая буква
- **THEN** 400 `exclude_q: не длиннее 100 символов`

### Requirement: Сводка по фильтру
При `summary=true` и хотя бы одном фильтре ответ `/operations` SHALL содержать
`summary` по ВСЕМ подходящим операциям периода, независимо от `limit` и курсора:
`first_date` и `last_date` (дата самой ранней и самой поздней операции) и
`by_category` — по строке `{category_uuid, category_name, total_eq, count}` на
категорию по убыванию `total_eq`; прямые переводы и долги — строка с
`category_uuid: null`. `total_eq` по правилу знака итога (с `kind` — сумма, без —
сальдо). Сумма `total_eq` и `count` по `by_category` равна `total_eq` и
`total_count` ответа. Если `summary=true` и задан `exclude_q`, `summary`
присутствует всегда: при нуле операций `first_date` и `last_date` равны `null`, а
`by_category` — пустой массив. Без `summary=true` или без фильтров блока нет.

#### Scenario: Границы дат шире страницы
- **WHEN** под фильтр подходят 70 операций, `limit=20`, `summary=true`
- **THEN** `first_date` — дата самой ранней из всех 70

#### Scenario: Две категории
- **WHEN** `q=стриж&kind=expense&summary=true`, «Стрижка» 23 в «Personal Care» и 15 в «Красота и уход»
- **THEN** `by_category` содержит обе категории с `total_eq` 23 и 15

#### Scenario: Ничего не найдено при exclude_q
- **WHEN** `q=zzz&exclude_q=a&summary=true`, подходящих операций нет
- **THEN** 200, `summary: {first_date: null, last_date: null, by_category: []}`

#### Scenario: Без summary
- **WHEN** `q=стриж`, `summary` не передан
- **THEN** в ответе нет `summary` и нет второго запроса к базе за агрегатом

### Requirement: Совпадения вне фильтра категорий
При `summary=true`, непустом `q` и `category_uuid` ответ SHALL содержать
`other_matches` — массив `{category_uuid, category_name, total_eq, count}` по
категориям, которых нет в `category_uuid`, где комментарий совпал с `q` (с учётом
`exclude_q`, `wallet_uuid`, `kind` и периода). Если таких нет, `other_matches` —
пустой массив, а не отсутствует. Без `q` или без `category_uuid`, а также при
`exclude_category_uuid`, `other_matches` в ответе нет.

#### Scenario: Стрижка в другой категории
- **WHEN** `q=стриж&category_uuid=<Personal Care>&summary=true`, «Стрижка» 15 лежит в «Красота и уход»
- **THEN** `other_matches` — `[{category_name: "Красота и уход", total_eq: "15", count: 1, …}]`, а `total_eq` ответа — только по «Personal Care»

#### Scenario: Нет совпадений вне фильтра
- **WHEN** те же параметры, других категорий с совпадением нет
- **THEN** `other_matches: []`

#### Scenario: Без category_uuid
- **WHEN** `q=стриж&summary=true`
- **THEN** в ответе нет `other_matches`
