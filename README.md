# wallet-report

Сервис отчётов — CQRS-lite query-сторона, выделенная из
[wallet-api](https://github.com/WalletTransactions/wallet-api) 2026-09-08 (см. "История" ниже). Читает ТУ ЖЕ
Postgres-базу, что и wallet-api, но отдельным процессом и ЕДИНСТВЕННОЙ
read-only ролью — токен не выпускает и не ротирует, только проверяет JWT по
JWKS `google-auth`, тем же приёмом, что wallet-api/wallet-gateway.

Полное описание HTTP API — [`openapi.yaml`](openapi.yaml). Сервис отдаёт его
и сам: `GET /openapi.yaml` — файл как есть, `GET /docs` — Swagger UI поверх
него (вендоренный статический бандл, `third_party/swagger-ui/`, без CDN — см.
его README) с формой "Try it out" (кнопка Authorize принимает `$ACCESS_TOKEN`
из примера ниже).

```
браузер/клиент --(Authorization: Bearer / cookie session)--> wallet-report
                        проверяет подпись по JWKS auth-сервиса
                        (google-auth), сам никого не логинит
                            │
                            ▼
                  ТА ЖЕ база, что у wallet-api — ролью
                  wallet_reader (только SELECT/EXECUTE
                  на отчётные SQL-функции)
```

## Шареная БД, разные роли

Это НЕ отдельная база и НЕ отдельное хранилище с денормализованными
проекциями (то, что дало бы "полный" CQRS с событиями между сервисами) — а
более лёгкий вариант: одна и та же схема, консистентность без задержки
репликации, миграции по-прежнему только в `wallet-api/migrations/`
(владеет ими wallet-api, см. его README). Разделение — по правам и деплою, не
по хранилищу:

- **wallet-api** — единственный писатель: роль без прав DDL (см. его README,
  "Своя БД"), но с `INSERT`/`UPDATE`/`DELETE` на домен
  `ledger`/`account`/`operation`/... через `/sync/push`.
- **wallet-report** (этот сервис) — роль `wallet_reader`
  (`wallet-api/schema/role/wallet_reader.sql`) физически не имеет прав `INSERT`/
  `UPDATE`/`DELETE` — не "код не пишет", а "процесс не может писать, даже
  теоретически". Пул соединений процесса — `AppState::reports_pool` (см.
  `app_state.hpp`, `REPORTS_DB_POOL_SIZE`) — все read-only, той же ролью;
  никакого второго, write-соединения тут нет вовсе, в отличие от прежнего
  `AppState::db` в wallet-api.

Выигрыш от разделения на процессы (а не только на роли/соединения внутри
одного процесса, как было раньше) — реальная граница: тяжёлый или ошибочный
отчётный запрос не делит память/крашится вместе с write-путём синка, и
редеплой одного не требует останавливать другой.

## Read-only по конструкции

`db::Database` этого сервиса — read-only не по проверке в коде, а потому что
роль подключения (`REPORTS_DATABASE_URL`) не имеет прав писать вообще —
Postgres отклонит любую попытку `INSERT`/`UPDATE`/`DELETE` на уровне самой
СУБД, даже если бы в отчётном коде когда-нибудь завёлся баг. Сервис
**никогда** не провизионирует леджер (в отличие от `/sync/*` wallet-api) —
пользователь, ни разу не синкавший ни с одного устройства, получает пустой
отчёт (`200`, а не `404`/`500` — "нет данных" не ошибка, а read-only роль и
не смогла бы создать леджер).

## Отчёты (`GET /api/reports/*`)

Тонкий HTTP-слой (`src/reports.cpp` — разбор query, JSON-ответ) поверх
тестируемой логики `src/reports_repo.cpp` и уже готовых SQL-функций
`wallet-api/schema/function/*.sql`.

**Обычная сессионная аутентификация** (`session::current_user`), без
`device_id` — отчёты нужны браузеру (SPA `wallet-web`) не хуже мобильного
клиента. Леджер резолвится из уже существующего `app_user`/`ledger_member`.

| Путь | Что отдаёт |
| --- | --- |
| `GET /api/reports/balance` | Баланс каждого кошелька леджера на дату |
| `GET /api/reports/income` / `/expense` | Доход/расход за период с разбивкой по счёту |
| `GET /api/reports/expense-structure` | Доля каждой категории в расходе за период, с необязательным сравнением периодов; у строки `op_count` — число операций |
| `GET /api/reports/expense-by-category-by-month` | Расход по категориям в разрезе календарных месяцев: все месяцы периода, у категории — итог, `op_count` и непустые месяцы. Итоги совпадают с `/expense-structure` (ноги перевода погашены в Transfer), а не с `/income-expense-by-month` |
| `GET /api/reports/income-expense-by-month` | Доход/расход по месяцам — экран "Обзор" |
| `GET /api/reports/expense-by-weekday` | Расход по дню недели — экран "Обзор" |
| `GET /api/reports/expense-biggest-moves` | Категории с наибольшим изменением расхода между двумя периодами — экран "Обзор" |
| `GET /api/reports/operations` | Операции периода страницами по курсору — экран "Список операций"; необязательные фильтры `category_uuid`, `exclude_category_uuid` (всё, кроме этих категорий; вместе с `category_uuid` — 400), `wallet_uuid` (списки через запятую), `kind`, `q` до пагинации, при фильтрах — итог `total_eq`/`total_incomplete`/`total_count` по всему периоду |
| `GET /api/reports/currencies` | Справочник валют — для выпадающего списка "Валюта эквивалента" и списка валют; у элемента `show_in_equivalent`, `is_own` (boolean), `icon_uuid`, `precision` (число) |
| `GET /api/reports/accounts` | Все живые кошельки и категории леджера (`kind`, `include_archived`) — для Telegram-бота |
| `GET /api/reports/icons` | Иконки, доступные леджеру: общий набор (`common`) и свои (`user`) — вкладка "Иконки" |

Общие query-параметры бо́льшей части ручек: `eq_currency` (ISO-код валюты
эквивалента, например `USD`, обязателен), `from`/`to` (период, включительны с
обеих сторон) либо `date` (у `/balance`, обороты строго ДО даты). Полный
контракт каждой ручки (коды ответов, форма JSON) — `openapi.yaml`.

```bash
curl "http://127.0.0.1:8090/api/reports/balance?date=2026-09-05&eq_currency=USD" \
  -H "Authorization: Bearer $ACCESS_TOKEN"
# -> {"accounts":[{"account_uuid":"...","account_name":"Наличные","currency_uuid":"...",
#     "symbol":"$","precision":2,"balance":"500.00","balance_eq":"500.00"}, ...],
#     "total_eq":"500.00","incomplete":false,"eq_currency_uuid":"..."}
```

`total_eq`/`incomplete` — агрегат по ВСЕМУ результату (window-функции, без
второго похода в базу): `incomplete: true` значит "для части строк не
нашлось курса в `eq_currency`, `total_eq` занижен".

## Админ: агрегаты по пользователям и «Посмотреть как клиент»

Две admin-only возможности поверх ровно тех же `/api/reports/*`, без
отдельного набора "админских" эндпоинтов:

- **`POST /api/reports/admin/users/stats`** `{"user_ids": [...]}` — агрегаты
  по переданным пользователям (число кошельков, число операций, даты
  первой/последней операции, число участников леджера), по одной строке на
  каждого из них, кто владеет леджером (см. `reports_repo::list_user_stats`).
  Считается на СУБД одним `GROUP BY` по таблице и только по леджерам этих
  пользователей. До 2026-10-11 это был `GET /api/reports/admin/users` по ВСЕМ
  владельцам базы с подзапросами на каждый леджер — на стенде с данными
  нагрузочного теста (4181 леджер по 3000 операций) 15,8 с и 1 МБ JSON ради
  двух строк таблицы; новый запрос там же — 22 мс. До 5000 id за запрос,
  каждый — UUID, иначе `400`. Только `Role::Admin` (`role` — claim
  проверенного JWT, тот же приём, что и у `GET /admin/users` в google-auth —
  `support` туда тоже не допущен). Путь живёт под `/api/reports/*`, а не
  `/admin/*`: это оставляет его на этом сервисе без правки маршрутизации
  wallet-gateway (`/admin/*` целиком уходит на google-auth, см. его README,
  "Порядок важен").
- **`?as_user_id=<uuid>`** — необязательный query-параметр у ЛЮБОГО
  отчётного эндпоинта. Обычный вызов его не передаёт вовсе и получает свой
  собственный леджер, как и раньше; `Role::Admin` может передать чужой
  `user_id` — тогда эндпоинт резолвит леджер УКАЗАННОГО пользователя, тем же
  кодом и той же формой ответа, без отдельной копии каждого отчёта под
  `/admin/...`. У любой другой роли параметр не игнорируется молча, а даёт
  `403` — иначе забытая проверка роли на кнопке SPA была бы не видна:
  выглядела бы как "почему-то показывает мои же данные", а не как явная
  ошибка (см. `reports.cpp::resolve_subject_user_id`).

Каждый вызов с `as_user_id` пишется в лог отдельной строкой
(`event: admin_view_as`, `admin_user_id`, `target_user_id`) — минимальный
аудит "кто из админов и чей леджер смотрел", раз это read-only просмотр
чужих финансовых данных.

SPA (`../wallet-web/site/app.js::renderAdmin()`) сначала берёт `GET
/admin/users` (google-auth), затем `POST /api/reports/admin/users/stats` по
id оттуда и сшивает их по `user_id` в
одну таблицу, и добавляет кнопку "Посмотреть как клиент" — она переоткрывает
экран "Отчёты" с `as_user_id` во всех запросах и баннером режима просмотра
(см. README wallet-web).

Осознанно НЕ реализовано этим заходом: сам просмотр ограничен `GET
/api/reports/*` (только отчёты) — экраны с записью (валютные пары,
участники леджера) администратору чужими руками не показываются и не
редактируются; полноценная имперсонация (отдельный JWT с `sub` целевого
пользователя) не заводилась намеренно — при read-only отчётах в ней нет
необходимости, а любой write-путь она заставила бы защищать отдельной
проверкой на каждом эндпоинте wallet-api.

## Как проверяется токен (без своего OAuth)

Ровно тот же приём, что в [wallet-api](https://github.com/WalletTransactions/wallet-api)/
[wallet-gateway](https://github.com/WalletTransactions/wallet-gateway):

1. Читает токен — сначала заголовок `Authorization: Bearer`, иначе cookie
   `session` (см. `session.hpp`).
2. Проверяет подпись **публичным ключом** auth-сервиса (RS256) — забирает
   его с `GET {AUTH_SERVICE_URL}/.well-known/jwks.json` и кэширует в памяти
   (см. `util/jwks.hpp`).
3. Если подпись верна и токен не истёк — достаёт `sub`/`email`/`name`/`role`
   из payload.

Никакого общего секрета в `.env` — только `AUTH_SERVICE_URL`.

## Доступ через гейтвей

[wallet-gateway](https://github.com/WalletTransactions/wallet-gateway) маршрутизирует `/api/reports/*` сюда, а
остальной `/api/*` (`/api/wallets`) и `/sync/*` — по-прежнему на wallet-api
(см. его README и `WALLET_REPORT_API_URL`/`WALLET_API_URL` в `.env`
гейтвея). Прямой поход в порт wallet-report (минуя гейтвей) всё ещё работает
для API-клиентов с `Authorization: Bearer` (curl, Postman).

## Версия

`GET /version`, строка лога при старте и `build_info` в `GET /metrics` —
общая схема для всех 4 C++-сервисов проекта, см. README google-auth,
"Версия" (файл `VERSION` + git-хэш, откуда берутся, как проставляются на
билде — код и приём идентичны). Там же — `build` (тег образа из
`--build-arg APP_BUILD`) и `started_at` (время запуска) в `GET /version`.

Релиз — git-тегами: `vX.Y.Z-rc.N` публикует образ в ghcr и выкатывает его
в test, `vX.Y.Z` на том же коммите — тот же образ в prod; `X.Y.Z` должен
совпадать с `VERSION`. Подробно — [README google-auth, «Версия»](https://github.com/WalletTransactions/google-auth#версия), «Релиз».

## Логирование

Общая схема для всех сервисов проекта (см. README google-auth, раздел
"Логирование" — код `src/util/log.*` идентичен): JSON-строка на запись,
папка `LOG_DIR` (по умолчанию `logs/` на Windows, stdout на Linux/в Docker),
файл на каждый календарный день, ротация по смене даты. `trace_id` —
сквозной id запроса браузера через gateway → этот сервис.

## Конфигурация

```bash
cp .env.example .env
```

- `AUTH_SERVICE_URL` — адрес auth-сервиса (google-auth), откуда тянутся
  публичные ключи для проверки токена.
- `REPORTS_DATABASE_URL` — ТА ЖЕ база, что у wallet-api, ролью
  `wallet_reader` (см. "Шареная БД, разные роли" выше). Логин/пароль этой
  роли не параметризованы через отдельные переменные — они зашиты литералом
  в саму роль (`wallet-api/schema/role/wallet_reader.sql`, "Пароль в
  репозитории. Это осознанно для локальной разработки").
- `REPORTS_DB_POOL_SIZE` — сколько соединений с этой ролью держать
  одновременно (по умолчанию `8`, см. `src/db.hpp::Pool`) — раньше сервис
  держал ровно одно на весь процесс, и один медленный отчёт блокировал
  вообще все остальные запросы на всё время своего выполнения.
- `PORT` — по умолчанию `8084` (у google-auth занят `8080`, у wallet-api —
  `8082`, у wallet-web — `8083`, у гейтвея — `8090`).
- `LOG_SQL_PARAMS`/`LOG_SQL_MASK_PARAMS` — см. README google-auth,
  "Логирование".

## Сборка и запуск

```bash
cmake -B build
cmake --build build --config Release
build/Release/wallet_report.exe   # Windows; на Linux — build/wallet_report
```

Или `scripts/windows/run-console.bat` / `install-service.bat` (NSSM) — те же
скрипты, что у остальных сервисов, см. README google-auth за подробностями.

Перед первым запуском убедитесь, что google-auth уже поднят на
`AUTH_SERVICE_URL`, а схема `wallet` накачена (миграции — забота wallet-api,
см. его README, "Своя БД") — `wallet-report` ничего не создаёт и не мигрирует
сам, роль `wallet_reader` тем более лишена DDL.

## История

Выделен 2026-09-08 из `wallet-api` (там жил как `src/reports.cpp` +
`src/reports_repo.cpp` поверх второго, read-only соединения `AppState::
reports_db` — см. git-историю wallet-api до этой даты, если нужны детали
происхождения самих отчётных SQL-функций). Причина выделения — CQRS-lite:
физическая невозможность для read-стороны держать write-креды, независимый
деплой/рестарт от `/sync/*`. Обсуждение и рассмотренные альтернативы (событие
+ денормализованное хранилище через брокер сообщений — сознательно
отклонено как избыточное для LAN-only MVP одного пользователя) — не в
репозитории, только в переписке с пользователем.
