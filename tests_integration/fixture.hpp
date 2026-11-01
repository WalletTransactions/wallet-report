// Общая обвязка интеграционных тестов — живое соединение с Postgres и
// минимальные фикстуры (личность/иконка), без которых ни одна таблица
// протокола синка не примет строку (везде FK на account/icon/…, см.
// wallet-api/schema/table/README.md, "Общий набор колонок").
//
// open() (DATABASE_URL, полный доступ) — ТОЛЬКО чтобы засеять данные через
// настоящий sync_repo::push_batch(), тем же путём, каким их в проде пишет
// wallet-api (см. README.md, "Почему push_batch, а не SQL") — сам сервис
// wallet_report этой ролью никогда не подключается, см. app_state.hpp.
// open_reports() (REPORTS_DATABASE_URL, wallet_reader) — ровно то
// соединение, которым реально читает продовый reports_repo.cpp.
//
// Никакой очистки между тестами нет и не нужна: каждый TEST заводит СВОЙ
// user_id/device_id/ledger_id (случайные uuid, см. make_identity) — тесты не
// делят состояние и не мешают друг другу, сколько раз подряд ни запускай
// этот бинарь против одной и той же базы (см. README.md, "Изоляция").
#pragma once

#include <cctype>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "db.hpp"
#include "provisioning.hpp"
#include "util/uuid.hpp"

namespace fixture {

inline std::string database_url() {
    const char* v = std::getenv("DATABASE_URL");
    if (v == nullptr || *v == '\0') {
        throw std::runtime_error(
            "DATABASE_URL не задан — интеграционные тесты wallet-report требуют "
            "живой Postgres с уже накаченной схемой (wallet-api/migrations/), "
            "ЭТУ переменную использует только сеялка теста (push_batch), не сам "
            "сервис. См. tests_integration/README.md, раздел \"Как запустить\".");
    }
    return v;
}

// shared_ptr, не значение и не unique_ptr — намеренно: test_push_batch.cpp
// гоняет push_batch() в отдельном потоке с таймаутом (регрессия на дедлок
// 2026-09-04), и при срабатывании таймаута этот поток невозможно ни
// присоединить (он сам навсегда держит mutex_ данного db::Database), ни
// безопасно бросить — держать db::Database живым до конца процесса через
// разделяемое владение дешевле и безопаснее, чем гарантировать корректное
// завершение потока, который по определению не отвечает. Нормальный
// (не зависший) путь освобождает объект как обычно, когда обе копии
// shared_ptr выходят из области видимости.
inline std::shared_ptr<db::Database> open() {
    return std::make_shared<db::Database>(database_url());
}

// Отдельное соединение ТОЙ ЖЕ базой ролью wallet_reader — только для
// test_reports.cpp (см. reports_repo.hpp, зачем в проде тоже отдельное
// соединение). Своя переменная окружения, а не парсинг DATABASE_URL: та же
// база, но именно ЭТА роль обязана реально иметь нужные GRANT (см.
// wallet-api/schema/role/wallet_reader.sql) — подмена другой ролью с более
// широкими правами замаскировала бы пробел в грантах, если бы он был.
inline std::string reports_database_url() {
    const char* v = std::getenv("REPORTS_DATABASE_URL");
    if (v == nullptr || *v == '\0') {
        throw std::runtime_error(
            "REPORTS_DATABASE_URL не задан — тесты reports_repo требуют отдельного "
            "подключения ролью wallet_reader к той же базе, что и DATABASE_URL. "
            "См. tests_integration/README.md.");
    }
    return v;
}

inline std::shared_ptr<db::Database> open_reports() {
    return std::make_shared<db::Database>(reports_database_url());
}

struct TestIdentity {
    std::string user_id;
    std::string device_id;
    std::string ledger_id;
};

// Свежий пользователь+устройство+леджер на каждый вызов — см. шапку файла.
inline TestIdentity make_identity(db::Database& db) {
    TestIdentity id;
    id.user_id = uuidgen::v4();
    id.device_id = uuidgen::v4();
    // "other" — единственное из допустимых значений device.platform (см.
    // CHECK-констрейнт в миграции), которое не претендует быть настоящей
    // платформой; интеграционным тестам это неважно, только не пусто.
    auto identity = provisioning::ensure_identity(db, id.user_id, "integration-test@example.com",
                                                   id.device_id, "integration-test-device", "other");
    id.ledger_id = identity.ledger_id;
    return id;
}

namespace detail {

// 64 hex-символа = 32 байта — ровно формат blob.sha256 (см. миграцию,
// CHECK octet_length(sha256) = 32). Один uuidgen::v4() даёт 32 hex-цифры
// (16 байт), поэтому склеиваем два разных uuid, не заботясь о крипто-
// стойкости — это фикстура, не настоящий хэш содержимого.
inline std::string random_hex64() {
    std::string hex;
    for (char c : uuidgen::v4() + uuidgen::v4()) {
        if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
    }
    return hex.substr(0, 64);
}

} // namespace detail

// uuid + hex sha256 разом — нужно тестам, которые проверяют, что
// icon_sha256 в отчёте (f_expanse_by_category_for_period, 000005) реально
// совпадает с content_sha256 ИМЕННО этой иконки, не просто "похоже на hex".
// make_reference_icon() ниже — обёртка для мест, которым нужен только uuid
// (большинство существующих тестов).
struct ReferenceIcon {
    std::string uuid;
    std::string sha256_hex;
};

// Заводит ОБЩУЮ (ledger_id IS NULL) иконку напрямую SQL, в обход push_batch —
// проще фикстуры, где нужен только САМ факт существующей иконки (account/
// currency без icon_uuid не примет ни один push), не разбор push icon/blob
// как таковых (см. test_push_batch.cpp, push_then_pull_round_trips_icon_*
// — те гоняют настоящий push_batch). Один и тот же способ годится для
// любого леджера — не привязан к конкретному user_id/ledger_id, поэтому не
// входит в TestIdentity.
inline ReferenceIcon make_reference_icon_with_sha256(db::Database& db) {
    ReferenceIcon out;
    out.uuid = uuidgen::v4();
    out.sha256_hex = detail::random_hex64();

    db.exec("INSERT INTO blob (sha256, bytes) VALUES (decode($1, 'hex'), decode('00', 'hex'))",
            {out.sha256_hex});

    auto seq_res = db.exec("SELECT next_seq('00000000-0000-0000-0000-000000000001')");
    db.exec("INSERT INTO icon (uuid, seq, name, content_sha256, icon_type) "
            "VALUES ($1, $2, $3, decode($4, 'hex'), 0)",
            {out.uuid, seq_res.rows.at(0).fields.at(0), "test-icon-" + out.uuid, out.sha256_hex});

    return out;
}

inline std::string make_reference_icon(db::Database& db) {
    return make_reference_icon_with_sha256(db).uuid;
}

// ОБЩАЯ (ledger_id IS NULL) валюта напрямую SQL — push_batch пишет только
// валюты леджера (ledger_id подставляется сервером, см. sync_repo.hpp), а
// тестам справочника валют нужна и общая строка (is_own: false, «своя
// перекрывает общую»). iso_code/name уникальны среди общих (uq_currency_*_reference),
// поэтому вызывающий передаёт уникальный код.
inline std::string make_reference_currency(db::Database& db, const std::string& iso_code,
                                           const std::string& icon_uuid, bool show_in_equivalent) {
    std::string uuid = uuidgen::v4();
    auto seq_res = db.exec("SELECT next_seq('00000000-0000-0000-0000-000000000001')");
    db.exec("INSERT INTO currency (uuid, seq, name, iso_code, icon_uuid, symbol, show_in_equivalent) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7)",
            {uuid, seq_res.rows.at(0).fields.at(0), "ref-" + iso_code, iso_code, icon_uuid, iso_code,
             show_in_equivalent ? "true" : "false"});
    return uuid;
}

} // namespace fixture
