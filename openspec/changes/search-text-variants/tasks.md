# Tasks

## 1. wallet-api: миграция

- [x] 1.1 `000032_operations_text_variants`: `p_qs text[] DEFAULT NULL` у `f_operations_for_period` и `f_operations_total` (DROP + CREATE, тела из 000030, `GRANT`, `COMMENT`); `down` — тела 000030 дословно. Зеркала `schema/function/f_operations_*.sql`, `GRANT` в `schema/role/wallet_reader.sql`. Проверка: `migrate goto 31` → `up` → `down 1` → `up` без ошибок; после `down` md5 определений, ACL и COMMENT всех функций `public` совпадают с `000031`; `schema/apply.sql` на пустой базе даёт те же определения и ACL двух функций, что миграции — выполнено 2026-11-06 на одноразовом Postgres 16 (не wallet-test): up/down/up без ошибок, md5 до/после `down` совпали (38170f51…), повторный `up` воспроизводим (1f857915…); `apply.sql` = миграции по md5 определения+ACL+COMMENT обеих функций. Попутно найдено: `GRANT` в `schema/role/wallet_reader.sql` ссылался на старую сигнатуру — исправлено

## 2. wallet-report

- [x] 2.1 `parse_operations_filter` принимает все значения `q`: пустые отбрасываются, каждый ≤ 100 символов, не больше 10 (`q: не больше 10 значений`); один — `q`, несколько — `qs`. Проверка: `TEST(operations_filter_parse_q_variants)` — выполнено 2026-11-06
- [x] 2.2 `append_filter_args`: `qs` → `p_qs => ARRAY[$n, …]::text[]`; `reports.cpp` собирает повторённый `q` (`get_param_value_count`). Проверка: `TEST(operations_filter_q_variants_match_any_and_count_once)` — регистр/ё/буквальность у вариантов, операция под двумя вариантами один раз, итог = итог одного `q`, И с кошельком, обход по курсору с `limit 2` без пропусков — выполнено 2026-11-06: тестовый образ против одноразового Postgres 16 с миграциями ветки — 48/48. `run_local.sh` против wallet-test не прошёл по окружению: пароль `wallet_reader` в скрипте (`wallet_reader:wallet_reader`) не совпадает с паролем роли в кластере wallet-test — 45 тестов не подключились к базе; к этому change не относится
- [x] 2.3 `openapi.yaml`: `q` — массив (`style: form`, `explode: true`, `maxItems: 10`), текст 400. Проверка: описание совпадает со spec — выполнено 2026-11-06 (сверено вручную)

## 3. Живой стек

- [ ] 3.1 После выкладки wallet-api (000032) и wallet-report на test: `GET /api/reports/operations?…&kind=expense&q=стриж&q=парикмах` через гейтвей возвращает операции с обоими словами, `total_count` = числу строк по всем страницам. Проверка: вывод curl приложен к тест-задаче

## Workflow follow-up

- Выкладка: сначала wallet-api (миграция), потом wallet-report, потом wallet-mcp (`search-operations-variants-and-paging`). Архивировать вместе с change wallet-mcp.
