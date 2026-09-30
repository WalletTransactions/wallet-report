# Интеграционные тесты wallet-report

Перенесены из `wallet-api/tests_integration/test_reports.cpp` при выделении
этого сервиса (CQRS-lite, см. README сервиса, "История") — против ЖИВОГО
Postgres со схемой wallet-api (шареная база, см. README, "Шареная БД, разные
роли"), не любой такой: с уже накаченными миграциями
`wallet-api/migrations/`.

Не собираются по умолчанию и не зарегистрированы в CTest — им нужен живой
Postgres, не фейк.

## Почему push_batch, а не SQL

Тесты сеют данные через `sync_repo::push_batch()` — ровно тот же протокол,
которым в проде пишет wallet-api, а не руками через SQL-вставки. Это не
только удобство: заодно подтверждает, что отчёты читают данные ТАК ЖЕ, как
их видит протокол синка, а не какой-то отдельный, потенциально
рассинхронизировавшийся способ записи. Отсюда — `sync_repo.cpp`/
`provisioning.cpp`/`util/hex.cpp`/`util/uuid.cpp` присутствуют в дереве этого
сервиса (`src/`), но НЕ входят в исполняемый файл `wallet_report` (см.
`../CMakeLists.txt`) — их единственный потребитель — тестовый бинарь
`wallet_report_integration_tests` (см. `CMakeLists.txt` рядом). Сервис,
который реально уходит в прод, не содержит ни строчки кода, способной
писать в базу.

## Что покрыто

`test_reports.cpp` — полный список тестов и что каждый проверяет см. в самом
файле (по одному TEST на функцию `reports_repo.hpp` + справочник валют).

## Изоляция

Каждый `TEST` заводит СВОИ `user_id`/`device_id`/`ledger_id` (случайные uuid,
см. `fixture::make_identity`) — тесты не делят состояние. Можно гонять этот
бинарь сколько угодно раз подряд против одной и той же базы.

## Как запустить

### Быстрый путь

```bash
bash run_local.sh
```

Один скрипт вместо ручных шагов ниже: заводит (если ещё нет) отдельную базу
`wallet_api_it` **в том же кластере Postgres**, что уже поднят вашим dev-
стеком (`podman compose ... up -d db`, см. `wallet-infra/README.md`) — не второй
контейнер, а просто ещё одна база рядом, накатывает миграции `wallet`
(идемпотентно), собирает тестовый образ (`Containerfile` рядом) и
прогоняет его. Повторные запуски быстрее — слои и база переиспользуются, а
тесты не требуют очистки между прогонами (см. "Изоляция" выше).

Запускать вручную перед коммитом, который правит схему или миграции
`wallet-api` (или Jenkins-job `wallet-report-integration-tests`).

### Вручную (тот же результат другим способом)

Нужен **отдельный, одноразовый** Postgres — не тот, что использует основной
`podman compose` стек. Тот же рецепт, что у wallet-api (см. его
`tests_integration/README.md`, "Как запустить", разделы 1-2 — поднять
одноразовый Postgres, накатить миграции `wallet`), с шагом 3 (сборка/запуск)
в контексте ЭТОГО каталога:

```bash
cmake -B build -DWALLET_REPORT_BUILD_INTEGRATION_TESTS=ON \
  && cmake --build build --target wallet_report_integration_tests
```

Нужны ОБЕ переменные окружения — сеялка теста пишет одной ролью, проверяемый
код читает другой:

```
DATABASE_URL=postgres://postgres:integration-test-only@<host>:5432/wallet_api_it?sslmode=disable
REPORTS_DATABASE_URL=postgres://wallet_reader:wallet_reader@<host>:5432/wallet_api_it?sslmode=disable
```

(`wallet_reader`/`wallet_reader` — та же роль, что заводит миграция
`000002_ledger_sync`, см. `wallet-api/schema/role/wallet_reader.sql`.)
