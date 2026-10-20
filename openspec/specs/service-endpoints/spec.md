# service-endpoints Specification

## Purpose
Служебная часть сервиса, общая с другими C++-сервисами проекта: liveness,
версия, метрики, самоописание API и сквозное логирование.

## Requirements

### Requirement: Liveness
`GET /healthz` SHALL без аутентификации отвечать 200 `{"status":"ok"}`,
проверяя только то, что процесс обслуживает HTTP; доступность БД не проверяется.

#### Scenario: Probe compose
- **WHEN** healthcheck вызывает `/healthz`
- **THEN** 200, а запрос не пишется в access-лог

### Requirement: Версия билда
`GET /version` SHALL без аутентификации возвращать `service`, `version` (из
файла `VERSION`) и `git_commit` (короткий хэш, `unknown` при сборке без git).
Те же значения SHALL отдаваться в `build_info` метрик и в строке лога при старте.

#### Scenario: Сборка без git
- **WHEN** сервис собран без информации о коммите
- **THEN** `git_commit` равен `unknown`

### Requirement: Метрики Prometheus
`GET /metrics` SHALL отдавать метрики, включая гистограмму HTTP-запросов по
методу, маршруту и статусу и счётчик `jwt_claims_total`. Маршрут в лейбле —
шаблон эндпоинта, у запросов без маршрута — `(unset)`.

#### Scenario: Скрейп
- **WHEN** Prometheus запрашивает `/metrics`
- **THEN** ответ содержит метрики, а запрос не пишется в access-лог

### Requirement: Самоописание API
`GET /openapi.yaml` SHALL отдавать файл спецификации как есть, `GET /docs` —
Swagger UI из вендоренного бандла `third_party/swagger-ui/` без обращения к CDN.

#### Scenario: Открытие документации
- **WHEN** браузер открывает `/docs`
- **THEN** загружается Swagger UI поверх `/openapi.yaml` только с ресурсов самого сервиса

### Requirement: Структурированные логи и trace_id
Сервис SHALL писать логи JSON-строками (stdout на Linux, по файлу в день в
`LOG_DIR`). `trace_id` SHALL браться из заголовка `X-Trace-Id`, выставленного
wallet-gateway, и возвращаться в ответе; сам сервис trace_id не порождает.

#### Scenario: Запрос через гейтвей
- **WHEN** запрос пришёл с заголовком `X-Trace-Id`
- **THEN** все записи лога этого запроса содержат этот `trace_id`, а ответ содержит тот же заголовок

#### Scenario: Завершение запроса
- **WHEN** запрос к API завершён
- **THEN** в лог пишется `request_finished` с методом, путём, статусом и `duration_ms`
