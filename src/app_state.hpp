// Состояние процесса wallet-report. В отличие от AppState wallet-api — НЕТ
// write-соединения к БД вообще: этот сервис физически не способен ничего
// записать, не только по коду, но и по конфигурации процесса (у него просто
// нет строки подключения с правом INSERT/UPDATE/DELETE, см. README,
// "Read-only по конструкции").
#pragma once

#include <memory>
#include <string>

#include "db.hpp"
#include "util/jwks.hpp"

struct AppState {
    // Публичные ключи auth-сервиса (RS256, GET /.well-known/jwks.json) —
    // ими проверяется подпись access-токена. Никакого общего секрета в
    // конфиге — см. README google-auth, "Как убрали общий секрет".
    std::unique_ptr<jwks::Cache> jwks;

    // Пул соединений с БД — той же самой, что и у wallet-api (шареная база,
    // см. README, "Шареная БД, разные роли"), ролью wallet_reader
    // (wallet-api/schema/role/wallet_reader.sql): только SELECT/EXECUTE на
    // отчётные SQL-функции. Раньше здесь была ЕДИНСТВЕННАЯ db::Database на
    // весь процесс (имя поля reports_db совпадало с AppState wallet-api не
    // просто так — это был тот же самый объект, переехавший в процесс, для
    // которого он единственный); по прямому запросу ("чини первый шаг" из
    // разбора самых проблемных запросов — один медленный отчёт держал
    // мьютекс ЕДИНСТВЕННОГО соединения и блокировал вообще все остальные
    // запросы, см. db.hpp::Pool) заменена на пул из REPORTS_DB_POOL_SIZE
    // (main.cpp) экземпляров Database — вызывающий код (reports.cpp) берёт
    // одно на время вызова через reports_pool->acquire().
    std::unique_ptr<db::Pool> reports_pool;
};
