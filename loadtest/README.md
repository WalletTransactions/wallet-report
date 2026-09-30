# Нагрузочный контур wallet-report (через wallet-gateway)

Проверяет, что переход `wallet-report` с одного соединения+мьютекс на пул
(`db::Pool`, `REPORTS_DB_POOL_SIZE`, см. `../src/db.hpp`) реально снимает
эффект "один тяжёлый отчёт блокирует всех остальных", и помогает подобрать
разумный размер пула.

Нагрузка идёт **через `wallet-gateway`** (порт `8090`), не напрямую в
`wallet-report` (`8084`) — так, как реально ходит браузер/мобильный клиент.
Из-за этого в измерение попадают ещё два фактора, которые сейчас НЕ
доработаны (см. "Как читать результаты" ниже):

- `wallet-api` (`../../wallet-api/src/db.hpp`) — всё ещё одно соединение +
  мьютекс, ровно тот паттерн, что был у wallet-report до фикса;
- `wallet-gateway` — на каждый проксируемый запрос создаёт новый
  `httplib::Client` без keep-alive (`../../wallet-gateway/src/main.cpp`) и
  сам работает на дефолтном thread-pool cpp-httplib (нигде не
  переопределён).

Этот контур их не чинит — только делает так, чтобы вклад пула
wallet-report был виден отдельно от их шума.

## Разбор пути запроса

```
k6 --(Bearer JWT)--> wallet-gateway:8090 --(JWT verify #1, свой JWKS-кэш)
                                          --(новый httplib::Client, без keep-alive)
                                          --> wallet-report:8084 --(JWT verify #2)
                                                                 --> db::Pool (REPORTS_DB_POOL_SIZE)
                                                                 --> Postgres (роль wallet_reader)
```

Гейтвей не переписывает путь (`out.path = req.target`) — значит k6 бьёт в
те же `/api/reports/...`, что описаны в `../openapi.yaml`, только через
`localhost:8090` вместо `localhost:8084`.

## Шаги 1-2. Засеять данные и смин­тить токены — одним скриптом

Ни на хосте, ни в контейнерах compose нет тулчейна C++ (cmake/g++) — оба
новых инструмента (`wallet_report_loadtest_seed`, `mint_loadtest_token`)
собираются и выполняются в одноразовом `debian:bookworm-slim` (те же
зависимости, что в `Containerfile` самих сервисов), тем же приёмом, что и
`../tests_integration/run_local.sh`: сеть/контейнер "db" находятся
динамически (имя сети зависит от имени каталога проекта — не хардкодится),
креды — из `infra/db/.env`, наружу не печатаются.

Сам `build-loadtest/` (и `../../google-auth/build-loadtest/`) собирается не
прямо в смонтированный `$WR_DIR`/`$GA_DIR`, а в именованный docker volume
(`wallet_report_loadtest_build`/`google_auth_loadtest_build`), наложенный
поверх этого пути — если репозиторий лежит на NTFS/exFAT (`fuseblk`,
частый случай для внешних дисков), такая ФС не хранит бит "исполняемый" для
обычных файлов: собранный бинарник выходит `-rw-r--r--`, и даже `chmod +x`
внутри контейнера эффекта не даёт (permission bit молча не меняется — реальный
случай, ловится только на `exec`: `Permission denied`). Volume хранится в
собственном сторадже движка контейнеров (обычно ext4/overlay2 хоста), где
exec-бит работает как положено; исходники и остальной CMake-кэш остаются на
обычном bind-mount. "Уже собрано" (кэш между запусками) определяется не
`test -x` по host-пути (файл в volume, с хоста не виден), а `docker/podman
volume inspect` + разовым `test -x` внутри контейнера с этим volume.

Требование: стек уже поднят (`podman compose --env-file infra/db/.env up -d`),
включая сервис `db`.

```bash
bash run.sh
```

Делает разом:

1. Собирает оба инструмента в `build-loadtest/` рядом с исходниками (только
   при первом запуске или если поправили C++-код — `LOADTEST_FORCE_REBUILD=1`
   форсирует пересборку).
2. Сеет данные **в ту же dev-базу**, что уже поднята стеком (роль
   `WALLET_POSTGRES_USER`, сеть контейнера `db`, хост `db:5432`) — НЕ в
   отдельную одноразовую базу, как `run_local.sh` — чтобы нагрузка потом
   била ровно в тот контейнер `wallet-report`, который вы тюните. По
   умолчанию — 5 синтетических пользователей (`LOADTEST_USERS`), у каждого
   3 кошелька (2×USD, 1×EUR), 5 категорий и 3000 операций
   (`LOADTEST_OPS_PER_USER`) за последние 2 года (`LOADTEST_DAYS_SPAN`).
   Пишет через настоящий `sync_repo::push_batch()` — тот же путь, которым в
   проде пишет wallet-api, а не сырой SQL (см.
   `../tests_integration/README.md`, "Почему push_batch, а не SQL"). Email
   синтетических пользователей — `loadtest-<n>@loadtest.example.com`: легко
   найти и выпилить потом, если понадобится.
3. Минтит по одному токену на пользователя (`mint_loadtest_token` — тот же
   код, что подписывает настоящие access-токены google-auth,
   `crypto::RsaSigningKey` + `jwt::sign_rs256`, читает реальный
   `google-auth/jwt_signing_key.pem` вашего dev-стека). **Только для
   локальной разработки** — не гоняйте против стенда с боевым ключом. TTL —
   24 часа по умолчанию (`TTL_SECS`), с запасом на весь прогон нагрузки.

Пример: `LOADTEST_USERS=1 LOADTEST_OPS_PER_USER=50 bash run.sh` — быстрый
прогон малым объёмом, чтобы проверить, что контур вообще работает, прежде
чем сеять полный объём.

Результат — `data/users.json` и `data/tokens.json`.

Проверка руками, что токен реально принимается (и гейтвеем, и
wallet-report):

```bash
# Без зависимости от jq на хосте (внутри run.sh он есть — в контейнере;
# здесь просто достаём первый токен из уже готового файла).
T=$(grep -o '"token": *"[^"]*"' data/tokens.json | head -1 | sed -E 's/.*"([^"]+)"$/\1/')
curl -H "Authorization: Bearer $T" http://localhost:8090/api/reports/currencies
# ожидается 200, не 401
```

## Шаг 3. Прогнать сценарии (k6)

Нет своего k6 — установите нативно, либо гоняйте контейнером в сети compose
(имя сети — `<имя_каталога_проекта>_wallet-net`, у стандартного чекаута это
`servers_wallet-net`; своё уточните через `podman network ls`, если не
совпадает, — тот же способ, что видит браузер через гейтвей, не зависит от
особенностей port-forwarding podman-machine):

```bash
podman run --rm --network servers_wallet-net \
  -v "$(pwd)/k6:/scripts:ro" \
  -v "$(pwd)/data:/data:ro" \
  -e GATEWAY_URL=http://wallet-gateway:8090 \
  -e TOKENS_FILE=/data/tokens.json \
  docker.io/grafana/k6 run /scripts/scenario_smoke.js
```

Нативно (проще для быстрой итерации, `8090` уже опубликован на хост):

```bash
GATEWAY_URL=http://localhost:8090 TOKENS_FILE=./data/tokens.json \
  k6 run k6/scenario_smoke.js
```

Сценарии (`k6/`), по нарастающей:

1. **`scenario_smoke.js`** — 1 VU, каждый эндпоинт по разу. Ворота перед
   серьёзной нагрузкой: убедиться, что всё отвечает `200` и валидным JSON,
   прежде чем гонять сотни VU.
2. **`scenario_dashboard_burst.js`** — главный сценарий: одна VU-итерация =
   одно открытие вкладки "Отчёты" = все ~11 эндпоинтов разом (`http.batch`,
   тот же паттерн, что описан в комментарии к `db::Pool`). Ступени VU:
   1 → 10 → 30 → 60.
3. **`scenario_pool_saturation.js`** — сустейн-нагрузка именно на
   `balance-series` (самый тяжёлый эндпоинт — пересчитывает бакеты с начала
   истории) при `VUS` (env, по умолчанию 20) одновременных запросах — это и
   есть сценарий, который чинил переход на пул.

## Шаг 4. Методика — REPORTS_DB_POOL_SIZE=1 vs 8 vs другие

`db::Pool` при `size=1` — это В ТОЧНОСТИ прежнее поведение (одно соединение,
тот же путь блокировки), тот же код, просто N=1. Поэтому "было / стало"
сравнивается ОДНОЙ переменной окружения, без отдельной сборки старой версии.

`wallet-report/.env` править не обязательно (и без доступа на чтение к нему
у ассистента иначе не проверить, что реально применилось) — есть
`../../compose.loadtest-override.yaml`: отдельный compose-файл поверх
основного, задаёт `REPORTS_DB_POOL_SIZE` только для одного пересоздания
контейнера, ничего не трогая в самом `.env`:

```bash
cd ../..   # servers/

# 1. "Как было" (эмуляция): применяем оверлей → REPORTS_DB_POOL_SIZE=1.
podman compose --env-file infra/db/.env -f compose.yaml -f compose.loadtest-override.yaml \
  up -d wallet-report
cd wallet-report/loadtest
podman run --rm --network servers_wallet-net --user "$(id -u):$(id -g)" \
  -v "$(pwd -W)/k6:/scripts:ro" -v "$(pwd -W)/data:/data" \
  -e GATEWAY_URL=http://wallet-gateway:8090 -e TOKENS_FILE=/data/tokens.json \
  -e VUS=15 -e DURATION=45s \
  docker.io/grafana/k6 run --summary-export=/data/result_pool1.json k6/scenario_pool_saturation.js

# 2. "Как стало" — снять оверлей (вернуться к обычному .env, дефолт 8):
cd ../..
podman compose --env-file infra/db/.env up -d wallet-report
cd wallet-report/loadtest
podman run --rm --network servers_wallet-net --user "$(id -u):$(id -g)" \
  -v "$(pwd -W)/k6:/scripts:ro" -v "$(pwd -W)/data:/data" \
  -e GATEWAY_URL=http://wallet-gateway:8090 -e TOKENS_FILE=/data/tokens.json \
  -e VUS=15 -e DURATION=45s \
  docker.io/grafana/k6 run --summary-export=/data/result_pool8.json k6/scenario_pool_saturation.js

# 3. Дополнительно 4/16/20 — поправить REPORTS_DB_POOL_SIZE в оверлее и
#    повторить, чтобы найти точку, после которой рост пула перестаёт давать
#    выигрыш. РЕАЛЬНЫЙ потолок — НЕ max_connections=100 Postgres (как
#    предполагалось раньше) и не thread-pool гейтвея/wallet-report, а
#    CONNECTION LIMIT 20 у самой роли wallet_reader (см.
#    wallet-api/migrations/000002_ledger_sync.up.sql) — REPORTS_DB_POOL_SIZE=32
#    воспроизводимо кладёт wallet-report в рестарт-луп ("too many connections
#    for role wallet_reader" в логах), НЕ ошибка нагрузочного контура,
#    настоящий инцидент (2026-09-17). Выше 20 пробовать бессмысленно — упадёт
#    всегда, вне зависимости от версии/конфигурации самого wallet-report.
```

Реальный прогон через `record_run.sh` (не разовые файлы, как выше — вся
серия ушла в `perf_history`/Grafana понятно названными ветками
`pool-size-1`/`pool-size-4`/`pool-size-8`/`pool-size-16`/`pool-size-20`,
VUS=15, 45s каждый, 2026-09-17):

| REPORTS_DB_POOL_SIZE | RPS | p50, мс | p90, мс | p95, мс | p99, мс | CPU wallet-report, % | занято conns |
|---|---|---|---|---|---|---|---|
| 1  | 8.05  | 1868 | 2420 | 2535 | 2916 | 0.80 | 1.00 |
| 4  | 27.40 | 551  | 657  | 738  | 1008 | 1.19 | 2.67 |
| 8 (текущий дефолт) | 34.57 | 372 | 693 | 866 | 1216 | 3.34 | 8.00 |
| 16 | 38.43 | 376 | 507 | **553** | 628 | 3.21 | 14.00 |
| 20 (потолок роли) | 38.43 | 372 | 514 | 561 | 639 | 5.36 | 11.25 |

**Вывод — оптимум 16, не 8 (текущий дефолт) и не 20 (потолок):** throughput
между 16 и 20 не растёт вовсе (38.43 у обоих — уже упёрлись не в пул, а в
что-то другое ниже по стеку, вероятно в сам `balance-series`, см. шапку
`db.hpp`), а CPU у 20 выше на треть без единого выигрыша взамен. 20 к тому же
сидит РОВНО на пределе роли — без запаса на случайное параллельное
подключение тем же `wallet_reader` (например, ручной `psql`/DBeaver) кладёт
сервис в тот же рестарт-луп. 16 даёт весь доступный выигрыш (p95 в 1.6 раза
короче дефолтных 8) и оставляет 4 соединения запаса под лимитом.

`--summary-export=...json` — сохраняет полную сводку метрик (не только то,
что видно в терминале) в JSON, чтобы сравнивать прогоны файлами, а не на
глаз. `--user "$(id -u):$(id -g)"` — официальный образ `grafana/k6` изнутри
работает под своим непривилегированным `k6` (uid 12345), который не может
писать в `-v .../data:/data`, смонтированную с правами хостового
пользователя (реальный случай — `Could not save some summary information:
... permission denied` при попытке `--summary-export`); флаг заставляет
контейнер писать от имени того же пользователя, что и владеет `data/`, — тот
же приём, которым уже пользуются `record_run.sh`/`record_wall_run.sh` и
Jenkinsfile. Реальный прогон (VUS=15, 45s, один и тот же `balance-series`):

| | `REPORTS_DB_POOL_SIZE=1` | `REPORTS_DB_POOL_SIZE=8` |
|---|---|---|
| req/s | 67.8 | 177.2 |
| запросов всего | 3 066 | 7 983 |
| avg | 219.8 мс | 82.4 мс |
| p50 | 185.7 мс | 72.7 мс |
| p90 | 295.4 мс | 122.9 мс |
| p95 | 378.9 мс | 142.3 мс |
| errors | 0 | 0 |

Пул даёт ×2.6 по throughput и p95 в 2.7 раза короче — эффект именно тот, что
обещан в комментарии к `db::Pool` (`../src/db.hpp`). Сравнить два готовых
`--summary-export`-файла числами, а не глазами:

```bash
python3 -c "
import json
for f in ['data/result_pool1.json', 'data/result_pool8.json']:
    m = json.load(open(f))['metrics']['http_req_duration']
    print(f, 'avg=%.1f p95=%.1f max=%.1f' % (m['avg'], m['p(95)'], m['max']))
"
```

## Как читать результаты

`wallet-report` уже логирует `duration_ms` на каждый запрос структурированной
JSON-строкой (`{"event":"request_finished","duration_ms":...}`, см.
`../src/main.cpp`) — это время БЕЗ гейтвея и его невторично-используемого
`httplib::Client`. Сравните:

- **k6-задержка** (через `localhost:8090` — включает 2× JWT verify + connect
  overhead гейтвея к апстриму + сам wallet-report);
- **`duration_ms` из логов wallet-report** (только сам сервис: JWT verify +
  ожидание `Pool::acquire()` + SQL).

Разница между ними — вклад гейтвея. Так эффект пула останется виден
отдельно, даже если полный путь через гейтвей и wallet-api смазывает
абсолютные цифры — это ожидаемо при выбранном контуре (через гейтвей), а не
брак замера.

Логи уже льются в OpenSearch (`../../infra/logging`, поднят вместе с
остальным compose) — но `wallet-report` там раньше НЕ ХВАТАЛО: whitelist
fluent-bit (`../../infra/logging/fluent-bit.conf`) был написан до появления
этого сервиса и включал только
`db|redis|google-auth|wallet-api|wallet-web|wallet-gateway` — `wallet-report`
в регэкспе не было вовсе, его логи молча дропались. Исправлено (добавлен в
whitelist, `podman restart servers-fluent-bit-1` подхватывает новый конфиг)
и живьём проверено — сейчас доходят.

Смотреть можно через Dashboards (`http://localhost:15601`), либо запросом
напрямую к OpenSearch (проще для скриптовой сверки):

```bash
curl -s "http://localhost:19200/wallet-logs/_search" -H "Content-Type: application/json" -d '{
  "size": 0,
  "query": {"bool": {"filter": [
    {"term": {"container_name.keyword": "servers-wallet-report-1"}},
    {"term": {"event.keyword": "request_finished"}},
    {"range": {"@timestamp": {"gte": "now-10m"}}}
  ]}},
  "aggs": {
    "p": {"percentiles": {"field": "duration_ms", "percents": [50, 90, 95, 99]}}
  }
}' | python3 -m json.tool
```

`container_name`/`event` — `text`-поля, для точного фильтра нужен суффикс
`.keyword` (иначе `term`-запрос молча вернёт 0 хитов).

## Performance DB — история прогонов, сравнение по времени (Grafana)

`compare_results.sh` выше сравнивает РОВНО два `--summary-export` JSON и
ничего не запоминает — годится для быстрой проверки "не деградировало ли
прямо сейчас", но не для вопроса "как p95 на 1000 клиентах менялся
последние два месяца" или "покажи мне все прогоны ветки `main`". Для этого —
`record_run.sh`: гоняет `scenario_pool_saturation.js` (см. его шапку — он
уже параметризован по числу клиентов через `VUS`) один раз НА КАЖДЫЙ уровень
concurrency из `CLIENT_LEVELS` и пишет результат в отдельную Postgres-базу
`perf_history` (`test_run`/`test_result`, см.
`../../infra/db/migrations/perf/000001_init.up.sql`) — тот же инстанс
Postgres, что и всё остальное (контейнер `db`), но своя роль/база, читает
`perf_reader`, пишет `PERF_POSTGRES_USER` (см. `infra/db/.env.example`).

Один раз — накатить миграцию (после того, как обычным порядком поднята "db"):

```bash
bash ../../infra/db/migrate.sh perf up
```

(или тем же job'ом в Jenkins — `BuildPlatform/jobs/db-migrate/`, параметр
`TARGET=perf`, см. `BuildPlatform/jenkins/README.md`; до этого job'а миграции
катились только руками.)

Дальше — как `run.sh`, но записывает историю, а не просто печатает числа:

```bash
CLIENT_LEVELS="10 30 60" DURATION_SECS=30 bash record_run.sh
# на CI/другой ветке — ENVIRONMENT/VERSION, коммит и ветка берутся из git сами:
ENVIRONMENT=ci VERSION=1.4.0 CLIENT_LEVELS="10 30 60 120" bash record_run.sh
```

Помимо RPS/latency/errors из k6, на каждый уровень заодно запрашивается
Prometheus (`../../infra/monitoring/`, стек уже поднят) за средним CPU/RAM
контейнера `wallet-report` ровно за окно этого уровня
(`avg_cpu_pct`/`avg_mem_bytes` в `test_result`) — если Prometheus недоступен,
это просто остаётся `NULL`, остальная запись не блокируется.

Тем же способом (тот же `PROMETHEUS_URL`, то же окно) — три метрики самого
Postgres, не только контейнера `wallet-report` (миграция
`000003_pg_metrics`, датчики фильтруются по `PG_TARGET_DB`/`PG_TARGET_USER`,
по умолчанию `wallet_api`/`wallet_reader` — именно пул `wallet-report`, не
пул `wallet-api` на той же базе):

- `avg_pg_active_conns` — среднее число занятых (`active`/`idle in
  transaction`) соединений reports_pool за окно — прямой сигнал по
  `scenario_pool_saturation.js`;
- `avg_pg_cache_hit_pct` — buffer cache hit ratio, % (`increase()` дельты за
  окно, не мгновенное отношение);
- `avg_pg_lock_waits` — среднее число не выданных (ожидающих) блокировок за
  окно — требует custom-запрос `locks_waiting` у `postgres-exporter` (см.
  `../../infra/monitoring/postgres-exporter/queries.yaml` и
  `--extend.query-path` в `../../compose.yaml`).

Сравнение baseline/current — не аргументы скрипта, а дропдауны в Grafana
(`http://localhost:13000`, дашборд **"Perf History — Baseline vs Current"**):
выбираете любые две ветки (или один и тот же коммит дважды, если нужно) —
дашборд сам берёт ПОСЛЕДНИЙ записанный прогон каждой и показывает таблицу
"метрика / baseline / current / деградация %", sweep-графики latency и
деградации по уровням concurrency, композитный "Performance Score" (эвристика,
не замена реальным метрикам — см. описание панели) и историю всех прогонов.
Датасорс `perf-history` и сам дашборд заводятся сами при первом старте
Grafana (`../../infra/monitoring/grafana/provisioning/`), руками добавлять
ничего не нужно.

**Сделано:** Jenkins-пайплайн (см. ниже) вызывает `record_run.sh` отдельной
стадией ("Record to Perf History (Grafana)") после `compare_results.sh` —
CI-прогоны `pool_saturation` тоже копятся в `perf_history`, не только
локальные (`CLIENT_LEVELS="$VUS"` — job-параметр как единственный уровень
sweep). Раньше `avg_cpu_pct`/`avg_mem_bytes` (и теперь — `avg_pg_*`) в этих
строках были всегда `NULL`: контейнер Jenkins не был в сети `wallet-net`,
Prometheus для него недоступен. Пайплайн теперь сам подключает себя к
`wallet-net` в стадии Preflight (`docker network connect`, идемпотентно) и
передаёт `record_run.sh` `PROMETHEUS_URL=http://prometheus:9090` (service-DNS
внутри этой сети, не `localhost:19090` — тот резолвится в loopback самого
контейнера Jenkins) — см. комментарии в самом Jenkinsfile. Там же —
`PG_METRICS_CSV`, отдельный CSV-график ("wallet-report loadtest: Postgres" в
Jenkins Plot Plugin) по трём метрикам Postgres, независимо от perf_history/Grafana.

### scenario_find_the_wall.js — отдельная запись (record_wall_run.sh)

`scenario_find_the_wall.js` (см. "Шаг 3" выше) — open-loop, сам нарастает
по стадиям (10 → 1500 rps) ЗА ОДИН запуск k6, поэтому не ложится в
sweep-модель `record_run.sh` (один вызов k6 НА КАЖДЫЙ уровень `CLIENT_LEVELS`).
Для него — `record_wall_run.sh`: тот же `test_run`/`test_result`, но ОДНА
строка на весь прогон (не sweep), с двумя специфичными для него колонками
(миграция `infra/db/migrations/perf/000002_wall_scenario.up.sql`):

- `target_rate_max` — пиковый целевой rps из stages сценария (параметр
  `TARGET_RATE_PEAK`, по умолчанию 1500 — держите в синхроне при правке
  stages в самом `.js`, автоматически не читается);
- `dropped_iterations` — встроенная k6-метрика: сколько итераций executor
  не смог запустить вовремя (нехватка VU под целевой темп) — главный сигнал
  "уже не успевает", раньше роста `error_rate`/latency.

`clients` в этой строке — не уровень concurrency (сценарий open-loop, VU
считает сам k6), а k6-метрика `vus_max` (сколько VU реально понадобилось) —
только для traceability, не для сравнения "по уровням", как у остальных
сценариев.

```bash
bash record_wall_run.sh
ENVIRONMENT=ci VERSION=1.4.0 BRANCH=main bash record_wall_run.sh
```

В Jenkins — параметр джобы `RUN_FIND_THE_WALL` (по умолчанию `false`,
включайте вручную: прогон тяжёлый и долгий, ~3 минуты).

## Jenkins — визуализация и сравнение прогонов

Всё описанное выше (шаги 1-4) автоматизировано в отдельном репозитории
**BuildPlatform** (сосед этого репозитория, не его часть — CI-инфраструктура
намеренно отделена от кода проекта) —
`BuildPlatform/jobs/wallet-report-loadtest/Jenkinsfile`. Пайплайн строит
инструменты, сеет данные, минтит токены, гоняет три k6-сценария (плюс
опционально четвёртый — `scenario_find_the_wall.js`, параметр джобы
`RUN_FIND_THE_WALL`), публикует HTML-отчёт и трендовые графики (Plot
Plugin) по каждому билду, сравнивает p95 с предыдущим успешным билдом
(`compare_results.sh`, всё ещё здесь, рядом с остальным контуром — в
сравнение НЕ входит `find_the_wall`, у него нет CI-порогов по замыслу),
помечая билд `UNSTABLE` при регрессии, и пишет `pool_saturation`
(+ `find_the_wall`, если включён) в `perf_history` (`record_run.sh`/
`record_wall_run.sh`, см. "Performance DB" выше) — та же история, что и
локальные прогоны, видна в Grafana по билдам CI.

Тот же приём (минтинг + k6 + `record_run.sh` в `perf_history`, свой
Jenkins-job) — у wallet-api:
`BuildPlatform/jobs/wallet-api-loadtest/Jenkinsfile`, контур —
`../../wallet-api/loadtest/README.md`.

Сам Jenkins и его настройка (образ, плагины, `.env`, заведение джобы) —
целиком в BuildPlatform, не здесь: `BuildPlatform/README.md` и
`BuildPlatform/jenkins/README.md`. Инструмент нагрузки остаётся k6 (уже
рабочий, живьём проверенный контур) — не JMeter: у Jenkins Performance
Plugin из коробки нет формата k6, поэтому визуализация — через Plot/HTML
Publisher, а не "нативный" performance-трекинг.

## Файлы

```
loadtest/
  README.md              — этот файл
  run.sh                  — шаги 1-2: сборка обоих инструментов + сев + минтинг, всё в Podman
  compare_results.sh      — инструмент сравнения: два --summary-export JSON -> таблица + код возврата
  extract_metrics.sh      — JSON -> CSV для Jenkins Plot Plugin
  render_report.sh        — JSON -> статический HTML-отчёт (HTML Publisher)
  record_run.sh           — Performance DB: sweep по CLIENT_LEVELS -> perf_history (Postgres), см. раздел выше
  record_wall_run.sh      — Performance DB: ОДНА запись на прогон scenario_find_the_wall.js, см. раздел выше
  .gitignore              — data/ и build-loadtest/ не коммитятся
  data/                   — users.json, tokens.json (генерируются run.sh), perf_*.json (генерируются record_run.sh)
  k6/
    lib/tokens.js          — SharedArray с токенами, round-robin по VU
    lib/endpoints.js       — таблица всех GET /api/reports/*
    lib/summary_options.js — общий summaryTrendStats (p50/p90/p95/p99 в --summary-export), шарится всеми сценариями
    scenario_smoke.js
    scenario_dashboard_burst.js
    scenario_pool_saturation.js — параметризован по VUS, используется record_run.sh для sweep

../../infra/db/migrations/perf/         — миграция test_run/test_result + роль perf_reader (000001) +
                                           target_rate_max/dropped_iterations под find_the_wall (000002) +
                                           avg_pg_active_conns/avg_pg_cache_hit_pct/avg_pg_lock_waits (000003)
                                           (см. infra/db/migrate.sh perf up, или Jenkins job db-migrate)
../../infra/monitoring/postgres-exporter/queries.yaml — custom-запрос locks_waiting для avg_pg_lock_waits (000003)
../../infra/monitoring/grafana/dashboards/perf-history.json — дашборд "Perf History — Baseline vs Current"

../tests_integration/seed_loadtest.cpp  — сеялка (опциональный CMake target, собирает run.sh)
../../google-auth/tools/mint_loadtest_token.cpp — минтер токенов (опциональный CMake target, собирает run.sh)

Jenkins и его настройка — в отдельном репозитории (см. "Jenkins" выше):
BuildPlatform/
  README.md
  compose.yaml
  jenkins/                — образ, плагины, .env, инструкции по настройке
  jobs/wallet-report-loadtest/Jenkinsfile — стадии Preflight -> ... -> Report -> Compare -> Record to Perf History
  jobs/wallet-api-loadtest/Jenkinsfile    — тот же приём, для ../../wallet-api/loadtest/
../../infra/jenkins/                    — сервис Jenkins (Containerfile, plugins.txt, README.md настройки)
../../compose.jenkins.yaml              — опциональный compose-оверлей, поднимающий Jenkins
```
