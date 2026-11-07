#include "sync_repo.hpp"

#include <algorithm>
#include <stdexcept>

using nlohmann::json;

namespace sync_repo {

const std::vector<TableSpec>& push_tables() {
    static const std::vector<TableSpec> tables = {
        {"account", Scope::LedgerOnly, {"name", "comment", "icon_uuid", "is_archived", "system_kind", "kind"}},
        {"account_operating", Scope::LedgerOnly, {"currency_uuid"}},
        // capital-assets — ровно как в wallet-api/src/sync_repo.cpp.
        {"account_asset", Scope::LedgerOnly, {"asset_type", "include_in_net_worth", "comment"}},
        {"operation", Scope::LedgerOnly,
         {"date", "acc_fr_uuid", "acc_to_uuid", "amount", "comment"}, {"amount"}},
        {"operation_link", Scope::LedgerOnly, {"main_uuid", "link_uuid", "type"}},
        // show_in_equivalent — необязательная (TableSpec::optional_columns),
        // как в wallet-api: строки без ключа получают DEFAULT true.
        {"currency", Scope::ReferenceOrLedger,
         {"name", "iso_code", "icon_uuid", "symbol", "precision", "show_in_equivalent"},
         {}, {}, {}, {}, "", {"show_in_equivalent"}},
        {"currency_pair", Scope::ReferenceOrLedger,
         {"symbol", "base_currency_uuid", "quote_currency_uuid", "precision"}},
        // conflict_columns/conflict_where — см. TableSpec, почему именно у
        // этой таблицы конфликт резолвится по (ledger, пара, дата), а не по
        // uuid, как у всех остальных.
        {"currency_pair_rate", Scope::ReferenceOrLedger,
         {"currency_pair_uuid", "date", "rate"}, {"rate"}, {}, {"date"},
         {"ledger_id", "currency_pair_uuid", "date"},
         "ledger_id IS NOT NULL AND deleted_at IS NULL"},
        {"currency_conversion", Scope::ReferenceOrLedger,
         {"from_currency_uuid", "to_currency_uuid", "num", "currency_pair_uuid", "is_reverse"}},
        // content_sha256 — hex-строка на границе протокола (см. TableSpec::
        // hex_columns и blob.hpp/README, "Иконки"); само содержимое сюда не
        // входит, только ссылка на уже загруженный blob.
        {"icon", Scope::ReferenceOrLedger,
         {"name", "content_sha256", "icon_type"}, {}, {"content_sha256"}},
    };
    return tables;
}

const std::vector<TableSpec>& pull_tables() {
    return push_tables();
}

namespace {

const TableSpec* find_spec(const std::vector<TableSpec>& tables, const std::string& name) {
    for (const auto& t : tables) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

// Порядок ПРИМЕНЕНИЯ push-батча — НЕ порядок push_tables() и уж тем более
// не порядок итерации `changes` (nlohmann::json — это std::map, ключи
// объекта отдаются в АЛФАВИТНОМ порядке, не в том, в котором их вставил
// клиент). Ссылающаяся строка (например, account.icon_uuid) обязана
// применяться ПОСЛЕ строки, на которую она ссылается — иначе FK-ошибка на
// batch, где клиент одним заходом создаёт несколько связанных сущностей
// (типичный случай: новая валюта + новый кошелёк в ней в одной офлайн-
// сессии). Реальная находка (2026-09-05, тест reports_repo, seed'ящий
// currency+account+account_operating одним батчем): "account_operating" <
// "currency" алфавитно — без этого списка INSERT в account_operating ушёл
// бы ДО currency и упал на 23503. Тот же порядок, что TABLE_ORDER на
// Android-клиенте (SyncEngine, применяет pull симметрично) — не совпадение,
// зависимости одни и те же в обе стороны.
constexpr const char* kApplyOrder[] = {
    "icon", "currency", "currency_pair", "currency_pair_rate", "currency_conversion",
    "account", "account_operating", "account_asset", "operation", "operation_link",
};

// JSON -> текстовый параметр SQL. Суммы/курсы приходят СТРОКАМИ уже на
// уровне протокола (см. wallet-api/schema/README.md, "Деньги — numeric") —
// сюда они попадают через ветку is_string() как есть, без разбора
// в число и обратно (что и вернуло бы туда же погрешность двоичной
// дроби, ради которой на сервере вообще завели numeric).
std::string scalar_to_param(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "t" : "f";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    // NULL/объект/массив на месте скалярной бизнес-колонки — конкретной
    // колонке об этом судить (NOT NULL constraint откажет сам, если поле
    // обязательно), здесь не диагностируем заранее.
    return v.dump();
}

// Один INSERT ... ON CONFLICT ... DO UPDATE на строку, колонки и их порядок
// — из TableSpec. ledger_id всегда АУТЕНТИФИЦИРОВАННЫЙ (параметр функции), а
// не из тела запроса — клиент не может писать ни чужой леджер, ни (для
// ReferenceOrLedger-таблиц) общую (ledger_id IS NULL) строку, см.
// sync_repo.hpp.
void upsert_row(db::Database::Transaction& tx, const TableSpec& spec, const std::string& ledger_id,
                const std::string& device_id, int64_t seq, const json& row) {
    std::string row_uuid = row.at("uuid").get<std::string>();
    bool deleted = row.value("deleted", false);

    std::vector<std::string> params = {ledger_id, row_uuid, std::to_string(seq), device_id,
                                        deleted ? "t" : "f"};

    std::string columns_sql = "ledger_id, uuid, seq, updated_by, deleted_at";
    // $5 — контекст CASE WHEN делает из текстового "t"/"f" boolean; now() —
    // время СЕРВЕРА в момент применения батча, не то, что мог прислать клиент
    // (deleted_at — только диагностика, см. README wallet-api/schema, "Даты").
    std::string values_sql = "$1, $2, $3, $4, CASE WHEN $5 THEN now() ELSE NULL END";
    // uuid = EXCLUDED.uuid — нужно ТОЛЬКО когда конфликт резолвится по
    // conflict_columns (естественный ключ, не uuid, см. TableSpec):
    // конфликтующая строка тогда физически другая (её uuid ещё не совпадал с
    // присланным), и именно последний push должен победить целиком, включая
    // uuid. При обычном конфликте по uuid это было бы самоприсвоением того
    // же значения — не вредно, но и не нужно, поэтому строка добавляется
    // только в этой ветке.
    std::string update_sql = spec.conflict_columns.empty() ? "" : "uuid = EXCLUDED.uuid, ";
    update_sql += "seq = EXCLUDED.seq, updated_by = EXCLUDED.updated_by, "
                  "deleted_at = EXCLUDED.deleted_at";

    int idx = 6;
    for (const auto& col : spec.columns) {
        // Необязательная колонка без ключа — вне INSERT и вне SET, см.
        // TableSpec::optional_columns.
        if (std::find(spec.optional_columns.begin(), spec.optional_columns.end(), col) !=
            spec.optional_columns.end()) {
            auto it = row.find(col);
            if (it == row.end()) continue;
            if (!it->is_boolean()) throw std::runtime_error(col + ": ожидается boolean");
        }
        bool is_hex = std::find(spec.hex_columns.begin(), spec.hex_columns.end(), col) !=
                      spec.hex_columns.end();
        bool is_date_only = std::find(spec.date_only_columns.begin(), spec.date_only_columns.end(),
                                       col) != spec.date_only_columns.end();
        std::string placeholder = "$" + std::to_string(idx++);
        columns_sql += ", " + col;
        // hex_columns — bytea, клиент присылает hex-строку (см. TableSpec::
        // hex_columns) — decode(...,'hex') превращает её обратно в байты на
        // границе SQL; params всё равно остаётся текстовым (см. db.hpp,
        // почему сырые байты параметром передавать нельзя).
        values_sql += ", " + (is_hex ? "decode(" + placeholder + ",'hex')" : placeholder);
        // Колонка естественного ключа (conflict_columns) — она же арбитр
        // ON CONFLICT, ей и так гарантированно равны NEW/EXCLUDED в момент
        // конфликта; включать её в SET было бы самоприсвоением того же
        // значения (не вредно, но незачем).
        bool is_conflict_key = std::find(spec.conflict_columns.begin(), spec.conflict_columns.end(),
                                          col) != spec.conflict_columns.end();
        if (!is_conflict_key) update_sql += ", " + col + " = EXCLUDED." + col;
        std::string param = scalar_to_param(row.at(col));
        // date_only_columns — обрезаем ЯВНО до "yyyy-MM-dd" (10 символов),
        // не полагаясь на неявный CAST text->date (см. TableSpec::
        // date_only_columns, почему): клиент прислал что-то с временем — тот
        // же батч не должен создать вторую строку на ту же (ledger,
        // currency_pair, дата) только из-за разного времени в хвосте строки.
        if (is_date_only && param.size() > 10) param.resize(10);
        params.push_back(std::move(param));
    }

    std::string conflict_target;
    if (!spec.conflict_columns.empty()) {
        // Естественный ключ вместо uuid (см. TableSpec::conflict_columns) —
        // сегодня только currency_pair_rate. conflict_where — предикат
        // частичного индекса-арбитра, СИНТАКСИЧЕСКИ как есть в его DDL
        // (Postgres требует точного совпадения текста, иначе индекс не
        // примет роль arbiter).
        conflict_target = "(";
        for (size_t i = 0; i < spec.conflict_columns.size(); ++i) {
            if (i > 0) conflict_target += ", ";
            conflict_target += spec.conflict_columns[i];
        }
        conflict_target += ")";
        if (!spec.conflict_where.empty()) conflict_target += " WHERE " + spec.conflict_where;
    } else {
        conflict_target = spec.scope == Scope::LedgerOnly ? "(ledger_id, uuid)" : "(uuid)";
    }
    std::string sql = "INSERT INTO " + spec.name + " (" + columns_sql + ") VALUES (" + values_sql +
                       ") ON CONFLICT " + conflict_target + " DO UPDATE SET " + update_sql;

    tx.exec(sql, params);
}

// Пересчитывает конфликты дублирующегося имени счёта заново, читая живую
// базу (а не сохраняя результат неудачной попытки) — см. sync_repo.hpp,
// PushOutcome, почему конфликт не кэшируется по batch_id.
std::vector<Conflict> diagnose_account_conflicts(db::Database& db, const std::string& ledger_id,
                                                  const json& changes) {
    std::vector<Conflict> conflicts;
    if (!changes.contains("account") || !changes.at("account").is_array()) return conflicts;

    for (const auto& row : changes.at("account")) {
        if (!row.contains("uuid") || !row.contains("name")) continue;
        std::string uuid = row.at("uuid").get<std::string>();
        std::string name = row.at("name").get<std::string>();
        auto res = db.exec(
            "SELECT uuid FROM account WHERE ledger_id = $1 AND name = $2 "
            "AND deleted_at IS NULL AND uuid <> $3",
            {ledger_id, name, uuid});
        if (!res.rows.empty()) {
            conflicts.push_back(Conflict{"account", "duplicate_name", uuid, res.rows[0].fields.at(0)});
        }
    }
    return conflicts;
}

} // namespace

PushOutcome push_batch(db::Database& db, const std::string& ledger_id, const std::string& device_id,
                        const std::string& batch_id, const json& changes) {
    // Идемпотентность — отдельный, обычный (не в транзакции батча) запрос:
    // конфликты сюда никогда не попадают (см. ниже), поэтому найденная
    // запись — всегда исход успешного применения, отдаём её текст как есть.
    auto existing = db.exec("SELECT response FROM sync_push_batch WHERE batch_id = $1", {batch_id});
    if (!existing.rows.empty()) {
        return PushOutcome{200, existing.rows[0].fields.at(0)};
    }

    try {
        auto tx = db.begin_transaction();
        auto seq_res = tx.exec("SELECT next_seq($1)", {ledger_id});
        int64_t seq = std::stoll(seq_res.rows.at(0).fields.at(0));

        // kApplyOrder, не changes.begin()/end() — см. её комментарий выше.
        for (const char* table_name : kApplyOrder) {
            if (!changes.contains(table_name)) continue;
            const json& value = changes.at(table_name);
            const TableSpec* spec = find_spec(push_tables(), table_name);
            // Неизвестное имя таблицы — клиент может быть новее сервера
            // (прислал сущность, которую этот релиз ещё не знает); игнор,
            // не 400 — иначе апгрейд протокола ломал бы старые сервера.
            if (spec == nullptr || !value.is_array()) continue;
            for (const auto& row : value) upsert_row(tx, *spec, ledger_id, device_id, seq, row);
        }

        json response_json = {{"seq", seq}, {"conflicts", json::array()}};
        std::string response_text = response_json.dump();
        // В ТОЙ ЖЕ транзакции, что и сами данные (не отдельным db.exec()
        // после tx.commit() — раньше было так, и это был самозаклин: tx
        // ещё жива на этой строке и держит mutex_ соединения до конца своей
        // области видимости, а db.exec() пытается взять тот же mutex_
        // заново, неповторно входимый std::mutex просто ждёт сам себя
        // навсегда). Заодно и правильнее по сути: если процесс упадёт между
        // применением данных и записью факта идемпотентности, отдельная
        // транзакция оставила бы окно, где повтор того же batch_id снова
        // применил бы уже применённые изменения.
        tx.exec("INSERT INTO sync_push_batch (batch_id, device_id, ledger_id, seq, response) "
                "VALUES ($1, $2, $3, $4, $5)",
                {batch_id, device_id, ledger_id, std::to_string(seq), response_text});

        tx.commit();

        return PushOutcome{200, response_text};
    } catch (const db::QueryError& e) {
        // Транзакция уже откачена (см. db::Database::Transaction — ROLLBACK
        // в деструкторе, если не дошли до commit()).
        if (e.sqlstate == "23505" && e.constraint_name == "uq_account_name") {
            auto conflicts = diagnose_account_conflicts(db, ledger_id, changes);
            json arr = json::array();
            for (const auto& c : conflicts) {
                arr.push_back({{"table", c.table}, {"reason", c.reason}, {"uuid", c.uuid},
                                {"existing_uuid", c.existing_uuid}});
            }
            return PushOutcome{409, json{{"conflicts", arr}}.dump()};
        }
        if (e.sqlstate == "23505") {
            // Другой уникальный индекс, чем uq_account_name — например,
            // icon.name (uq_icon_name_reference/uq_icon_name_ledger, см.
            // wallet-api/schema/table/icon.sql) или currency.iso_code. Сервер не
            // диагностирует ЭТИ конфликты так же осмысленно (нет
            // существующего uuid, на который клиент мог бы переклеиться —
            // см. открытый вопрос в org/docs/TODO_mobile_sync.md), но 500 тут
            // хуже: клиент не отличил бы ожидаемый дубль от настоящего бага.
            // Осмысленный 409 без автоматического разрешения.
            return PushOutcome{409, json{{"error", "duplicate_key"},
                                          {"constraint", e.constraint_name}}
                                          .dump()};
        }
        if (e.sqlstate == "23503") {
            // Внешний ключ — например, счёт ссылается на icon_uuid, которого
            // сервер ещё не знает (порядок применения — сначала /sync/blobs,
            // потом строка icon, см. sync_repo.hpp) либо на acc_fr_uuid/
            // acc_to_uuid, ещё не отправленный в этом или прошлых батчах.
            return PushOutcome{409, json{{"error", "missing_reference"},
                                          {"detail", std::string(e.what())}}
                                          .dump()};
        }
        throw;
    }
}

namespace {

// Общий каркас запроса на один поток изменений — см. sync_repo.hpp,
// "Курсор и страницы": WITH candidates — все строки новее курсора по всем
// таблицам потока; cutoff — seq строки на границе limit; внешний SELECT
// добирает ВСЕ строки с seq <= cutoff, а не только первые limit — так группа
// с одинаковым seq (один push-батч) никогда не разрезается между двумя
// страницами. COALESCE(MAX(seq), $cursor) — если кандидатов нет вообще,
// курсор остаётся тем же, а не съезжает на NULL.
struct CandidateQuery {
    std::string sql;
    std::vector<std::string> params;
};

CandidateQuery build_candidates(const std::vector<TableSpec>& tables, bool ledger_scoped) {
    std::string sql = "WITH candidates AS (";
    bool first = true;
    for (const auto& t : tables) {
        if (ledger_scoped) {
            // Обе области видимости участвуют в потоке леджера — "свои"
            // копии reference-таблиц (заполненный ledger_id) едут вместе с
            // остальными его данными, см. README wallet-api/schema, "Потоков два".
        } else if (t.scope != Scope::ReferenceOrLedger) {
            continue; // в потоке справочников только то, что бывает общим
        }
        if (!first) sql += " UNION ALL ";
        first = false;

        // to_jsonb(t.*) сериализует numeric как голое JSON-число — для
        // amount/rate это в точности та потеря точности, ради которой
        // сервер вообще завёл numeric (см. TableSpec::numeric_columns); а
        // bytea (TableSpec::hex_columns, например icon.content_sha256) —
        // как "\x..." текст, неудобный клиенту без собственного парсинга.
        // jsonb-конкатенация (||) переопределяет только эти ключи, остальные
        // колонки остаются как есть.
        std::string data_expr = "to_jsonb(" + t.name + ".*)";
        if (!t.numeric_columns.empty() || !t.hex_columns.empty()) {
            std::string patch = "jsonb_build_object(";
            bool first_field = true;
            for (const auto& col : t.numeric_columns) {
                if (!first_field) patch += ", ";
                first_field = false;
                patch += "'" + col + "', " + col + "::text";
            }
            for (const auto& col : t.hex_columns) {
                if (!first_field) patch += ", ";
                first_field = false;
                patch += "'" + col + "', encode(" + col + ",'hex')";
            }
            patch += ")";
            data_expr = "(" + data_expr + " || " + patch + ")";
        }

        sql += "SELECT '" + t.name + "' AS tbl, seq, " + data_expr + " AS data FROM " + t.name +
               " WHERE ";
        sql += ledger_scoped ? "ledger_id = $1 AND seq > $2" : "ledger_id IS NULL AND seq > $1";
    }
    sql += ")";
    return {sql, {}};
}

PullResult run_pull(db::Database& db, const std::string& sql, std::vector<std::string> params,
                     int64_t cursor) {
    auto res = db.exec(sql, params);

    PullResult result;
    result.next_seq = cursor;
    result.changes = json::object();
    for (const auto& row : res.rows) {
        const std::string& tbl = row.fields.at(0);
        int64_t seq = std::stoll(row.fields.at(1));
        json data = json::parse(row.fields.at(2));
        if (!result.changes.contains(tbl)) result.changes[tbl] = json::array();
        result.changes[tbl].push_back(std::move(data));
        if (seq > result.next_seq) result.next_seq = seq;
    }
    return result;
}

} // namespace

PullResult pull_ledger_stream(db::Database& db, const std::string& ledger_id, int64_t cursor,
                               int limit) {
    auto candidates = build_candidates(pull_tables(), /*ledger_scoped=*/true);
    std::string sql = candidates.sql +
                       ", cutoff AS (SELECT COALESCE(MAX(seq), $2) AS max_seq FROM "
                       "(SELECT seq FROM candidates ORDER BY seq LIMIT $3) top_n) "
                       "SELECT tbl, seq, data FROM candidates, cutoff "
                       "WHERE candidates.seq <= cutoff.max_seq ORDER BY seq";
    return run_pull(db, sql, {ledger_id, std::to_string(cursor), std::to_string(limit)}, cursor);
}

PullResult pull_reference_stream(db::Database& db, int64_t cursor, int limit) {
    auto candidates = build_candidates(pull_tables(), /*ledger_scoped=*/false);
    std::string sql = candidates.sql +
                       ", cutoff AS (SELECT COALESCE(MAX(seq), $1) AS max_seq FROM "
                       "(SELECT seq FROM candidates ORDER BY seq LIMIT $2) top_n) "
                       "SELECT tbl, seq, data FROM candidates, cutoff "
                       "WHERE candidates.seq <= cutoff.max_seq ORDER BY seq";
    return run_pull(db, sql, {std::to_string(cursor), std::to_string(limit)}, cursor);
}

} // namespace sync_repo
