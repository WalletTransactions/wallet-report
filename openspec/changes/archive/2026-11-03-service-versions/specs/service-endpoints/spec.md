# Spec Delta

## MODIFIED Requirements

### Requirement: Версия билда
`GET /version` SHALL без аутентификации возвращать `service`, `version` (из
файла `VERSION`), `build` (тег образа из `APP_BUILD`, без него `<VERSION>-dev`),
`git_commit` (короткий хэш, `unknown` при сборке без git) и `started_at` (время
запуска процесса, ISO-8601 UTC). `service`, `version`, `build` и `git_commit`
SHALL отдаваться в `build_info` метрик и в строке лога при старте, а время
запуска — в `process_start_time_seconds`.

#### Scenario: Сборка без git
- **WHEN** сервис собран без информации о коммите
- **THEN** `git_commit` равен `unknown`

#### Scenario: Сборка без тега
- **WHEN** образ собран без `APP_BUILD`
- **THEN** `build` равен `<VERSION>-dev`

#### Scenario: Время запуска
- **WHEN** `/version` вызван дважды без перезапуска, а затем после перезапуска контейнера
- **THEN** первые два `started_at` равны, третий — время нового запуска
