#include "reports_repo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

using nlohmann::json;

namespace reports_repo {

namespace {

// Одна строка результата функции — в JSON по имени колонки, не по позиции
// (порядок в SQL и в columns обязан совпадать, но так опечатка в одном
// месте не молча сдвинет значения по другим ключам).
json row_to_json(const std::vector<std::string>& fields, const std::vector<std::string>& columns) {
    json row = json::object();
    for (std::size_t i = 0; i < columns.size(); ++i) row[columns[i]] = fields.at(i);
    return row;
}

} // namespace

std::optional<std::string> find_ledger_for_user(db::Database& db, const std::string& user_id) {
    auto res = db.exec(
        "SELECT ledger_id FROM ledger_member WHERE user_id = $1 ORDER BY joined_at LIMIT 1",
        {user_id});
    if (res.rows.empty()) return std::nullopt;
    return res.rows[0].fields.at(0);
}

std::optional<std::string> resolve_eq_currency(db::Database& db, const std::string& iso_code,
                                                const std::string& ledger_id) {
    auto res = db.exec(
        "SELECT uuid FROM currency WHERE iso_code = $1 AND deleted_at IS NULL "
        "AND (ledger_id IS NULL OR ledger_id = $2) "
        "ORDER BY (ledger_id IS NULL) LIMIT 1",
        {iso_code, ledger_id});
    if (res.rows.empty()) return std::nullopt;
    return res.rows[0].fields.at(0);
}

CurrenciesResult list_currencies(db::Database& db, const std::string& ledger_id) {
    // DISTINCT ON (iso_code) ... ORDER BY iso_code, (ledger_id IS NULL) —
    // тот же приоритет "своя строка вместо общей при совпадении кода", что и
    // resolve_eq_currency выше, только сразу для целого списка: (ledger_id
    // IS NULL) = false (своя) сортируется ПЕРЕД true (общая), значит
    // DISTINCT ON оставляет для этого iso_code именно свою строку.
    //
    // JOIN icon — по прямому запросу ("флаг страны" в выпадающем списке):
    // currency.icon_uuid NOT NULL REFERENCES icon(uuid) (см. table/currency.sql,
    // "Иконка-флаг/символ этой валюты" — комментарий колонки буквально про
    // это), значит иконка есть гарантированно, JOIN, не LEFT JOIN. Сама
    // картинка — в blob.sha256 (icon.content_sha256), которую веб-клиент
    // качает как есть через уже существующий GET /sync/blobs/:sha256; hex —
    // тот же приём (encode(...,'hex')), что и icon_sha256 в
    // f_expanse_by_category_for_period.sql у категорий.
    //
    // Без леджера (ledger_id пуст, см. .hpp) — отдельный текст запроса без
    // параметра вовсе: $2, типизированный контекстом как uuid, не принял бы
    // пустую строку (см. остальные функции файла, почему они не вызываются
    // с пустым ledger_id вовсе, а не в том, что не пытались).
    //
    // show_in_equivalent/is_own/icon_uuid/precision (change currency-ui-tweaks)
    // — флаг пикера эквивалента, «своя/общая» и текущие иконка и точность для
    // формы правки на вебе. Список не фильтруется по флагу: тот же справочник
    // нужен целиком пикерам валютных пар, фильтрует клиент. is_own — это
    // (ledger_id IS NOT NULL): в запросе без леджера всегда false.
    static const char* kSelect =
        "c.uuid, c.iso_code, c.name, c.symbol, encode(i.content_sha256, 'hex'), "
        "c.show_in_equivalent, (c.ledger_id IS NOT NULL), c.icon_uuid, c.\"precision\" "
        "FROM currency c JOIN icon i ON i.uuid = c.icon_uuid ";
    auto res = ledger_id.empty()
        ? db.exec(std::string("SELECT ") + kSelect +
                  "WHERE c.deleted_at IS NULL AND c.ledger_id IS NULL "
                  "ORDER BY c.iso_code")
        : db.exec(std::string("SELECT DISTINCT ON (c.iso_code) ") + kSelect +
                      "WHERE c.deleted_at IS NULL AND (c.ledger_id IS NULL OR c.ledger_id = $1) "
                      "ORDER BY c.iso_code, (c.ledger_id IS NULL)",
                  {ledger_id});

    CurrenciesResult result;
    static const std::vector<std::string> kColumns = {
        "uuid", "iso_code", "name", "symbol", "icon_sha256",
        "show_in_equivalent", "is_own", "icon_uuid", "precision",
    };
    for (const auto& row : res.rows) {
        json out_row = row_to_json(row.fields, kColumns);
        // Драйвер отдаёт всё текстом ("t"/"f", "2") — признаки настоящими
        // JSON boolean, точность числом (тот же приём, что у list_accounts).
        out_row["show_in_equivalent"] = row.fields.at(5) == "t";
        out_row["is_own"] = row.fields.at(6) == "t";
        out_row["precision"] = std::stoi(row.fields.at(8));
        result.currencies.push_back(out_row);
    }
    return result;
}

AccountsResult list_accounts(db::Database& db, const std::string& ledger_id, AccountKindFilter kind,
                             bool include_archived) {
    // Фильтры — параметрами ($2/$3), а не склейкой текста запроса: один план
    // на все сочетания, и никакой ветки, где значение попадает в SQL строкой.
    // LEFT JOIN currency — валюта кошелька; у категории её нет. Удалённая
    // валюта (deleted_at) не прячет кошелёк: он живой и участвует в расчётах,
    // просто iso_code/symbol берутся как есть из tombstone-строки.
    std::string kind_param = kind == AccountKindFilter::Wallet     ? "wallet"
                             : kind == AccountKindFilter::Category ? "category"
                                                                   : "";
    auto res = db.exec(
        "SELECT a.uuid, a.name, "
        "CASE WHEN ao.uuid IS NULL THEN 'category' ELSE 'wallet' END AS kind, "
        "a.system_kind, a.is_archived, encode(i.content_sha256, 'hex'), "
        "ao.currency_uuid, c.iso_code, c.symbol, c.\"precision\" "
        "FROM account a "
        "JOIN icon i ON i.uuid = a.icon_uuid "
        "LEFT JOIN account_operating ao "
        "  ON ao.ledger_id = a.ledger_id AND ao.uuid = a.uuid AND ao.deleted_at IS NULL "
        "LEFT JOIN currency c ON c.uuid = ao.currency_uuid "
        "WHERE a.ledger_id = $1 AND a.deleted_at IS NULL "
        "AND ($2::text = '' OR ($2::text = 'wallet') = (ao.uuid IS NOT NULL)) "
        "AND ($3::boolean OR NOT a.is_archived) "
        "ORDER BY (ao.uuid IS NULL), a.name",
        {ledger_id, kind_param, include_archived ? "true" : "false"});

    static const std::vector<std::string> kColumns = {
        "uuid", "name", "kind", "system_kind", "is_archived", "icon_sha256",
        "currency_uuid", "iso_code", "symbol", "precision",
    };
    AccountsResult out;
    for (const auto& row : res.rows) {
        json out_row = row_to_json(row.fields, kColumns);
        // Тот же приём, что is_archived у wallet_turnover_for_period: признаки
        // — настоящими JSON bool/числом, а не строками "t"/"0".
        out_row["is_archived"] = row.fields.at(4) == "t";
        out_row["system_kind"] = std::stoi(row.fields.at(3));
        out.accounts.push_back(out_row);
    }
    return out;
}

ReportResult balance_to_date(db::Database& db, const std::string& ledger_id,
                              const std::string& date, const std::string& eq_currency_uuid) {
    // COALESCE(sum(...) OVER (), 0)/bool_or(...) OVER () — агрегат по ВСЕМУ
    // результату в каждой строке, без второго похода в базу (см.
    // reports_repo.hpp).
    auto result = db.exec(
        "SELECT account_uuid, account_name, currency_uuid, symbol, \"precision\", "
        "balance, balance_eq, "
        "COALESCE(sum(balance_eq) OVER (), 0)::text AS total_eq, "
        "bool_or(balance_eq IS NULL) OVER () AS incomplete "
        "FROM f_balance_to_date($1, $2, $3)",
        {ledger_id, date, eq_currency_uuid});

    static const std::vector<std::string> kColumns = {
        "account_uuid", "account_name", "currency_uuid", "symbol", "precision",
        "balance",      "balance_eq",
    };
    ReportResult out;
    for (const auto& row : result.rows) out.rows.push_back(row_to_json(row.fields, kColumns));
    if (!result.rows.empty()) {
        out.total_eq = result.rows[0].fields.at(7);
        out.incomplete = result.rows[0].fields.at(8) == "t";
    }
    return out;
}

namespace {

// income/expense устроены одинаково — единственная разница: имя SQL-функции.
ReportResult period_report(db::Database& db, const std::string& function_name,
                            const std::string& ledger_id, const std::string& from,
                            const std::string& to, const std::string& eq_currency_uuid) {
    std::string sql =
        "SELECT account_uuid, account_name, currency_uuid, amount, amount_eq, "
        "COALESCE(sum(amount_eq) OVER (), 0)::text AS total_eq, "
        "bool_or(amount_eq IS NULL) OVER () AS incomplete "
        "FROM " +
        function_name + "($1, $2, $3, $4)";
    auto result = db.exec(sql, {ledger_id, from, to, eq_currency_uuid});

    static const std::vector<std::string> kColumns = {
        "account_uuid", "account_name", "currency_uuid", "amount", "amount_eq",
    };
    ReportResult out;
    for (const auto& row : result.rows) out.rows.push_back(row_to_json(row.fields, kColumns));
    if (!result.rows.empty()) {
        out.total_eq = result.rows[0].fields.at(5);
        out.incomplete = result.rows[0].fields.at(6) == "t";
    }
    return out;
}

} // namespace

ReportResult income_for_period(db::Database& db, const std::string& ledger_id,
                                const std::string& from, const std::string& to,
                                const std::string& eq_currency_uuid) {
    return period_report(db, "f_income_for_period", ledger_id, from, to, eq_currency_uuid);
}

ReportResult expense_for_period(db::Database& db, const std::string& ledger_id,
                                 const std::string& from, const std::string& to,
                                 const std::string& eq_currency_uuid) {
    return period_report(db, "f_expanse_for_period", ledger_id, from, to, eq_currency_uuid);
}

ReportResult expense_by_category_for_period(db::Database& db, const std::string& ledger_id,
                                              const std::string& from, const std::string& to,
                                              const std::string& eq_currency_uuid) {
    // Тот же приём агрегата через OVER(), что и в period_report()/
    // balance_to_date() выше, но своя реализация, а не переиспользование
    // period_report(): у f_expanse_by_category_for_period нет колонок
    // currency_uuid/amount вовсе (см. её .sql — валюты уже смешаны через
    // amount_eq), под другой набор колонок пришлось бы городить условия в
    // общей функции ради одного вызывающего.
    auto result = db.exec(
        "SELECT account_uuid, account_name, amount_eq, icon_sha256, op_count, "
        "COALESCE(sum(amount_eq) OVER (), 0)::text AS total_eq, "
        "bool_or(amount_eq IS NULL) OVER () AS incomplete "
        "FROM f_expanse_by_category_for_period($1, $2, $3, $4)",
        {ledger_id, from, to, eq_currency_uuid});

    // icon_sha256 — hex-строка content_sha256 иконки категории (см. .sql:
    // encode(...,'hex') там же, а не здесь — единственный вызывающий, не
    // общий механизм TableSpec::hex_columns протокола синка). Веб-клиент
    // собирает по ней URL GET /sync/blobs/:sha256 сам (см. reports_repo.hpp).
    //
    // op_count (add-category-month-analytics) — JSON-числом, как precision у
    // остальных отчётов: число операций в сумме строки, у Transfer — переводов.
    static const std::vector<std::string> kColumns = {
        "account_uuid", "account_name", "amount_eq", "icon_sha256", "op_count",
    };
    ReportResult out;
    for (const auto& row : result.rows) {
        json out_row = row_to_json(row.fields, kColumns);
        out_row["op_count"] = std::stoll(row.fields.at(4));
        out.rows.push_back(out_row);
    }
    if (!result.rows.empty()) {
        out.total_eq = result.rows[0].fields.at(5);
        out.incomplete = result.rows[0].fields.at(6) == "t";
    }
    return out;
}

namespace {

// ""  (SQL NULL, см. db.cpp) -> nullopt; текст, который не парсится как
// число, тоже nullopt — быть щедрее тут незачем, оба случая рисуются
// одинаково ("~"/null) на клиенте.
std::optional<double> parse_opt_double(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        return std::stod(s);
    } catch (...) {
        return std::nullopt;
    }
}

// Один знак после запятой — тот же формат, что и у "п.п." в дизайне
// (Design/.../Отчёты - веб.dc.html). См. reports_repo.hpp про то, почему
// double здесь допустим (не деньги, только для интерфейса).
std::string format_pct(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
}

} // namespace

StructureResult expense_structure_for_period(db::Database& db, const std::string& ledger_id,
                                              const std::string& from, const std::string& to,
                                              const std::string& eq_currency_uuid,
                                              const std::optional<std::string>& cmp_from,
                                              const std::optional<std::string>& cmp_to) {
    StructureResult out;

    auto now_r = expense_by_category_for_period(db, ledger_id, from, to, eq_currency_uuid);
    out.total_eq = now_r.total_eq;
    out.incomplete = now_r.incomplete;
    double total_now = parse_opt_double(now_r.total_eq).value_or(0.0);

    bool has_cmp = cmp_from.has_value() && cmp_to.has_value() && !cmp_from->empty() && !cmp_to->empty();

    // uuid -> amount_eq периода сравнения (nullopt — курса не нашлось, ТА ЖЕ
    // строка есть, просто её сумму нельзя посчитать; отсутствие ключа вовсе —
    // категория в cmp-периоде не встречалась, это 0, не "неизвестно").
    std::unordered_map<std::string, std::optional<double>> before_amount;
    std::unordered_map<std::string, json> before_row_by_uuid;
    std::vector<std::string> before_order; // порядок ORDER BY из SQL — для before-only строк ниже
    double total_before = 0.0;

    if (has_cmp) {
        auto before_r = expense_by_category_for_period(db, ledger_id, *cmp_from, *cmp_to, eq_currency_uuid);
        out.compared = true;
        out.total_eq_before = before_r.total_eq;
        out.incomplete = out.incomplete || before_r.incomplete;
        total_before = parse_opt_double(before_r.total_eq).value_or(0.0);
        for (const auto& row : before_r.rows) {
            std::string uuid = row.at("account_uuid").get<std::string>();
            before_amount[uuid] = parse_opt_double(row.at("amount_eq").get<std::string>());
            before_row_by_uuid[uuid] = row;
            before_order.push_back(uuid);
        }
    }

    std::unordered_set<std::string> seen;
    for (const auto& row : now_r.rows) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        seen.insert(uuid);

        json out_row = row; // account_uuid/account_name/amount_eq как есть
        auto now_val = parse_opt_double(row.at("amount_eq").get<std::string>());
        std::optional<double> pct_now =
            (now_val && total_now > 0.0) ? std::optional<double>(*now_val / total_now * 100.0) : std::nullopt;
        out_row["pct"] = pct_now ? json(format_pct(*pct_now)) : json(nullptr);

        if (out.compared) {
            // Категория, которой не было в cmp-периоде вовсе, — это честный 0
            // (а не "неизвестно"), в отличие от amount_eq == nullopt (курс не
            // нашёлся) у СУЩЕСТВУЮЩЕЙ строки — см. комментарий у before_amount.
            auto it = before_amount.find(uuid);
            std::optional<double> before_val = it != before_amount.end() ? it->second : std::optional<double>(0.0);
            std::optional<double> pct_before = (before_val && total_before > 0.0)
                                                    ? std::optional<double>(*before_val / total_before * 100.0)
                                                    : std::nullopt;
            out_row["pct_before"] = pct_before ? json(format_pct(*pct_before)) : json(nullptr);
            out_row["pp_delta"] =
                (pct_now && pct_before) ? json(format_pct(*pct_now - *pct_before)) : json(nullptr);
        }
        out.categories.push_back(out_row);
    }

    // Категории, которые встречались в cmp-периоде, но пропали в основном, —
    // без этого прохода "куда делась статья" была бы не видна в сравнении
    // вообще (см. reports_repo.hpp у StructureResult).
    if (out.compared) {
        for (const auto& uuid : before_order) {
            if (seen.count(uuid)) continue;
            seen.insert(uuid);

            const auto& before_row = before_row_by_uuid.at(uuid);
            json out_row = {
                {"account_uuid", uuid},
                {"account_name", before_row.at("account_name")},
                {"amount_eq", "0"},
                {"icon_sha256", before_row.at("icon_sha256")}, // иконка категории не меняется между периодами
                {"op_count", 0}, // в основном периоде операций категории нет
            };
            out_row["pct"] = (total_now > 0.0) ? json(format_pct(0.0)) : json(nullptr);

            std::optional<double> before_val = before_amount.at(uuid);
            std::optional<double> pct_before = (before_val && total_before > 0.0)
                                                    ? std::optional<double>(*before_val / total_before * 100.0)
                                                    : std::nullopt;
            out_row["pct_before"] = pct_before ? json(format_pct(*pct_before)) : json(nullptr);
            out_row["pp_delta"] = pct_before ? json(format_pct(0.0 - *pct_before)) : json(nullptr);
            out.categories.push_back(out_row);
        }
    }

    return out;
}

CategoryByMonthResult expense_by_category_by_month(db::Database& db, const std::string& ledger_id,
                                                   const std::string& from, const std::string& to,
                                                   const std::string& eq_currency_uuid) {
    CategoryByMonthResult out;

    // Все месяцы периода — тем же generate_series, что у f_income_expense_by_month
    // (design D1: пустые клетки матрицы из базы не тянем, список месяцев один).
    auto months = db.exec(
        "SELECT to_char(m, 'YYYY-MM-DD') FROM generate_series("
        "date_trunc('month', $1::timestamp), date_trunc('month', $2::timestamp), interval '1 month') AS m",
        {from, to});
    for (const auto& row : months.rows) out.months.push_back(row.fields.at(0));

    // Строки уже упорядочены функцией: категории по итогу DESC, внутри — по
    // месяцу, поэтому новая категория начинается там, где сменился account_uuid.
    // total_eq — из функции (сумма итогов категорий в SQL), incomplete — bool_or
    // по всем клеткам тоже в SQL (design D2).
    auto result = db.exec(
        "SELECT account_uuid, account_name, icon_sha256, month_start, amount_eq, op_count, incomplete, "
        "category_amount_eq, category_op_count, total_eq::text, "
        "bool_or(incomplete) OVER () "
        "FROM f_expense_by_category_by_month($1, $2, $3, $4)",
        {ledger_id, from, to, eq_currency_uuid});

    auto nullable = [](const std::string& s) { return s.empty() ? json(nullptr) : json(s); };
    std::string current;
    for (const auto& row : result.rows) {
        const auto& f = row.fields;
        if (out.categories.empty() || f.at(0) != current) {
            current = f.at(0);
            out.categories.push_back({
                {"account_uuid", f.at(0)},
                {"account_name", f.at(1)},
                {"icon_sha256", f.at(2)},
                {"amount_eq", nullable(f.at(7))},
                {"op_count", std::stoll(f.at(8))},
                {"months", json::array()},
            });
        }
        out.categories.back()["months"].push_back({
            {"month_start", f.at(3)},
            {"amount_eq", nullable(f.at(4))},
            {"op_count", std::stoll(f.at(5))},
            {"incomplete", f.at(6) == "t"},
        });
    }
    if (!result.rows.empty()) {
        out.total_eq = result.rows[0].fields.at(9);
        out.incomplete = result.rows[0].fields.at(10) == "t";
    }
    return out;
}

MonthlyResult income_expense_by_month(db::Database& db, const std::string& ledger_id,
                                       const std::string& from, const std::string& to,
                                       const std::string& eq_currency_uuid) {
    auto result = db.exec(
        "SELECT month_start, income_eq, expense_eq, incomplete "
        "FROM f_income_expense_by_month($1, $2, $3, $4)",
        {ledger_id, from, to, eq_currency_uuid});

    MonthlyResult out;
    for (const auto& row : result.rows) {
        out.months.push_back({
            {"month_start", row.fields.at(0)},
            {"income_eq", row.fields.at(1)},
            {"expense_eq", row.fields.at(2)},
        });
        if (row.fields.at(3) == "t") out.incomplete = true;
    }
    return out;
}

WeekdayResult expense_by_weekday(db::Database& db, const std::string& ledger_id,
                                  const std::string& from, const std::string& to,
                                  const std::string& eq_currency_uuid) {
    auto result = db.exec(
        "SELECT isodow, expense_eq, incomplete "
        "FROM f_expense_by_weekday_for_period($1, $2, $3, $4)",
        {ledger_id, from, to, eq_currency_uuid});

    WeekdayResult out;
    for (const auto& row : result.rows) {
        out.days.push_back({
            {"isodow", std::stoi(row.fields.at(0))},
            {"expense_eq", row.fields.at(1)},
        });
        if (row.fields.at(2) == "t") out.incomplete = true;
    }
    return out;
}

BiggestMovesResult expense_biggest_moves(db::Database& db, const std::string& ledger_id,
                                          const std::string& from, const std::string& to,
                                          const std::string& cmp_from, const std::string& cmp_to,
                                          const std::string& eq_currency_uuid, int limit) {
    BiggestMovesResult out;

    // Переиспользуем ПЛОСКИЙ список по категории (не expense_structure_for_period
    // — той нужны проценты от total_eq, здесь важна только абсолютная сумма
    // каждой категории в обоих периодах) — дважды, тем же приёмом мёржа по
    // account_uuid, что и структура выше.
    auto now_r = expense_by_category_for_period(db, ledger_id, from, to, eq_currency_uuid);
    auto before_r = expense_by_category_for_period(db, ledger_id, cmp_from, cmp_to, eq_currency_uuid);
    out.incomplete = now_r.incomplete || before_r.incomplete;

    std::unordered_map<std::string, json> before_by_uuid;
    for (const auto& row : before_r.rows) before_by_uuid[row.at("account_uuid").get<std::string>()] = row;

    struct Candidate {
        json row;
        double abs_delta;
    };
    std::vector<Candidate> candidates;
    std::unordered_set<std::string> seen;

    for (const auto& row : now_r.rows) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        seen.insert(uuid);
        std::string now_amt = row.at("amount_eq").get<std::string>();
        auto it = before_by_uuid.find(uuid);
        // Категории не было в cmp-периоде вовсе — честный 0, не "неизвестно"
        // (тот же принцип, что перед этим уже применён в expense_structure_for_period).
        std::string before_amt = it != before_by_uuid.end() ? it->second.at("amount_eq").get<std::string>() : "0";
        auto now_v = parse_opt_double(now_amt);
        auto before_v = parse_opt_double(before_amt);
        if (!now_v || !before_v) continue; // курса не нашлось хотя бы в одном периоде — дельту не посчитать
        json out_row = {
            {"account_uuid", uuid},
            {"account_name", row.at("account_name")},
            {"icon_sha256", row.at("icon_sha256")},
            {"amount_now_eq", now_amt},
            {"amount_before_eq", before_amt},
        };
        candidates.push_back({out_row, std::abs(*now_v - *before_v)});
    }
    // Категории, которых нет "сейчас", но были "до" — тоже движение (обнулились).
    for (const auto& row : before_r.rows) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        if (seen.count(uuid)) continue;
        std::string before_amt = row.at("amount_eq").get<std::string>();
        auto before_v = parse_opt_double(before_amt);
        if (!before_v) continue;
        json out_row = {
            {"account_uuid", uuid},
            {"account_name", row.at("account_name")},
            {"icon_sha256", row.at("icon_sha256")},
            {"amount_now_eq", "0"},
            {"amount_before_eq", before_amt},
        };
        candidates.push_back({out_row, std::abs(0.0 - *before_v)});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.abs_delta > b.abs_delta; });
    if (static_cast<int>(candidates.size()) > limit) candidates.resize(static_cast<std::size_t>(limit));
    for (const auto& c : candidates) out.rows.push_back(c.row);

    return out;
}

namespace {

constexpr std::size_t kMaxFilterUuids = 50;
constexpr std::size_t kMaxQueryChars = 100;
constexpr std::size_t kMaxQueryValues = 10; // вариантов q (search-text-variants)

bool is_uuid_text(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

// Список uuid через запятую. Пустой элемент (в том числе хвостовая запятая)
// — та же ошибка, что не-uuid; дубли допустимы.
std::optional<std::string> parse_uuid_list(const std::string& name, const std::string& raw,
                                           std::vector<std::string>& out) {
    out.clear();
    if (raw.empty()) return std::nullopt;
    std::size_t pos = 0;
    while (true) {
        std::size_t comma = raw.find(',', pos);
        std::string piece = raw.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!is_uuid_text(piece)) return name + ": некорректный uuid";
        out.push_back(piece);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (out.size() > kMaxFilterUuids) return name + ": не больше 50 значений";
    return std::nullopt;
}

// Число кодовых точек корректной UTF-8 строки; nullopt — строка не UTF-8
// (битую строку Postgres отверг бы с 22021, то есть 500). Проверка строгая —
// без overlong-форм и суррогатов, те же правила, что у Postgres.
std::optional<std::size_t> utf8_length(const std::string& s) {
    std::size_t count = 0;
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        auto b = static_cast<unsigned char>(s[i]);
        std::size_t len;
        unsigned char lo = 0x80, hi = 0xBF; // допустимый диапазон второго байта
        if (b < 0x80) {
            len = 1;
        } else if (b >= 0xC2 && b <= 0xDF) {
            len = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            len = 3;
            if (b == 0xE0) lo = 0xA0;
            if (b == 0xED) hi = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            len = 4;
            if (b == 0xF0) lo = 0x90;
            if (b == 0xF4) hi = 0x8F;
        } else {
            return std::nullopt;
        }
        if (b == 0) return std::nullopt; // NUL в тексте Postgres не примет
        if (i + len > n) return std::nullopt;
        for (std::size_t k = 1; k < len; ++k) {
            auto c = static_cast<unsigned char>(s[i + k]);
            unsigned char min = k == 1 ? lo : 0x80;
            unsigned char max = k == 1 ? hi : 0xBF;
            if (c < min || c > max) return std::nullopt;
        }
        i += len;
        ++count;
    }
    return count;
}

std::string uuid_array_literal(const std::vector<std::string>& uuids) {
    std::string out = "{";
    for (std::size_t i = 0; i < uuids.size(); ++i) {
        if (i) out += ',';
        out += uuids[i];
    }
    out += '}';
    return out;
}

// Хвост вызова функции с фильтрами — именованной нотацией и ТОЛЬКО заданные
// (design D1): непереданный параметр получает DEFAULT NULL в самой базе,
// а db::Database::exec() SQL NULL передать не умеет. Значения идут
// параметрами $n, из фиксированного набора имён собирается только текст.
void append_filter_args(const OperationsFilter& filter, std::string& args, std::vector<std::string>& params) {
    auto next = [&params]() { return "$" + std::to_string(params.size() + 1); };
    if (!filter.category_uuids.empty()) {
        args += ", p_category_uuids => " + next() + "::uuid[]";
        params.push_back(uuid_array_literal(filter.category_uuids));
    }
    if (!filter.exclude_category_uuids.empty()) {
        args += ", p_exclude_category_uuids => " + next() + "::uuid[]";
        params.push_back(uuid_array_literal(filter.exclude_category_uuids));
    }
    if (!filter.wallet_uuids.empty()) {
        args += ", p_wallet_uuids => " + next() + "::uuid[]";
        params.push_back(uuid_array_literal(filter.wallet_uuids));
    }
    if (!filter.kind.empty()) {
        args += ", p_kind => " + next();
        params.push_back(filter.kind);
    }
    if (!filter.q.empty()) {
        args += ", p_q => " + next();
        params.push_back(filter.q);
    }
    // Варианты — отдельными $n в ARRAY[...], а не литералом '{...}': текст
    // произвольный (запятые, кавычки, фигурные скобки), литерал массива
    // пришлось бы экранировать, параметры — нет.
    if (!filter.qs.empty()) {
        std::string items;
        for (const auto& v : filter.qs) {
            items += (items.empty() ? "" : ", ") + next();
            params.push_back(v);
        }
        args += ", p_qs => ARRAY[" + items + "]::text[]";
    }
}

} // namespace

std::optional<std::string> parse_operations_filter(const std::string& category_uuid,
                                                    const std::string& exclude_category_uuid,
                                                    const std::string& wallet_uuid,
                                                    const std::string& kind,
                                                    const std::vector<std::string>& q_values,
                                                    OperationsFilter& out) {
    out = OperationsFilter{};
    if (auto err = parse_uuid_list("category_uuid", category_uuid, out.category_uuids)) return err;
    if (auto err = parse_uuid_list("exclude_category_uuid", exclude_category_uuid, out.exclude_category_uuids)) {
        return err;
    }
    // «Только эти, кроме этих» не нужен ни одному клиенту — ошибка проще
    // правила приоритета (design D4).
    if (!out.category_uuids.empty() && !out.exclude_category_uuids.empty()) {
        return std::string("category_uuid и exclude_category_uuid нельзя задавать вместе");
    }
    if (auto err = parse_uuid_list("wallet_uuid", wallet_uuid, out.wallet_uuids)) return err;
    if (!kind.empty() && kind != "expense" && kind != "income" && kind != "transfer") {
        return std::string("kind должен быть expense, income или transfer");
    }
    out.kind = kind;
    std::vector<std::string> qs;
    for (const auto& q : q_values) {
        if (q.empty()) continue;
        auto len = utf8_length(q);
        if (!len || *len > kMaxQueryChars) return std::string("q: не длиннее 100 символов");
        qs.push_back(q);
    }
    if (qs.size() > kMaxQueryValues) return std::string("q: не больше 10 значений");
    if (qs.size() == 1) {
        out.q = qs[0];
    } else {
        out.qs = std::move(qs);
    }
    return std::nullopt;
}

OperationsTotal operations_total(db::Database& db, const std::string& ledger_id, const std::string& from,
                                 const std::string& to, const std::string& eq_currency_uuid,
                                 const OperationsFilter& filter) {
    std::vector<std::string> params = {ledger_id, from, to, eq_currency_uuid};
    std::string args = "$1, $2, $3, $4";
    append_filter_args(filter, args, params);
    auto result = db.exec("SELECT total_eq, incomplete, matched FROM f_operations_total(" + args + ")", params);

    OperationsTotal out;
    if (!result.rows.empty()) { // функция отдаёт ровно одну строку; пусто — защитно
        const auto& f = result.rows[0].fields;
        out.total_eq = f.at(0).empty() ? "0" : f.at(0);
        out.incomplete = f.at(1) == "t";
        out.matched = std::stoll(f.at(2));
    }
    return out;
}

OperationsResult operations_for_period(db::Database& db, const std::string& ledger_id,
                                        const std::string& from, const std::string& to,
                                        const std::string& eq_currency_uuid, int limit,
                                        const std::string& cursor_date, const std::string& cursor_uuid,
                                        const OperationsFilter& filter) {
    static const char* kSelectColumns =
        "operation_uuid, \"date\", kind, amount, currency_uuid, symbol, \"precision\", "
        "amount_eq, comment, category_uuid, category_name, icon_sha256, wallet_uuid, wallet_name, "
        "from_uuid, from_name, to_uuid, to_name";
    static const std::vector<std::string> kColumns = {
        "operation_uuid", "date",         "kind",         "amount",      "currency_uuid",
        "symbol",         "precision",    "amount_eq",    "comment",     "category_uuid",
        "category_name",  "icon_sha256",  "wallet_uuid",  "wallet_name", "from_uuid",
        "from_name",      "to_uuid",      "to_name",
    };
    constexpr std::size_t kAmountEqIdx = 7; // индекс "amount_eq" в kColumns — для проверки incomplete

    // +1 — "подглядываем" на одну строку вперёд запрошенного limit: если она
    // пришла, значит после отданной страницы есть ещё хотя бы одна операция
    // (has_more), а саму эту лишнюю строку наружу не отдаём (см. take ниже).
    // Без этого пришлось бы либо отдельным запросом (COUNT/EXISTS) спрашивать
    // "а дальше есть ещё", либо врать has_more=true даже когда ровно limit
    // строк и были последними в периоде.
    std::string fetch_limit = std::to_string(limit + 1);

    // Первая страница — БЕЗ $6/$7 вовсе (не NULL-параметрами): Postgres
    // сам подставит DEFAULT NULL у p_cursor_date/p_cursor_uuid (см.
    // f_operations_for_period.sql) — db::Database::exec() пока не умеет
    // посылать настоящий SQL NULL через $N (все параметры — непустые
    // строки, см. db.hpp), тот же приём, что и у resolve_eq_currency с
    // пустым ledger_id (см. её комментарий в reports_repo.hpp).
    //
    // Курсор остаётся позиционным ($6, $7), фильтры — именованными после него
    // (append_filter_args). Так без фильтров текст вызова тот же, что до
    // миграции wallet-api 000027, и лента работает и на старой сигнатуре
    // функции (design, Risks).
    std::vector<std::string> params = {ledger_id, from, to, eq_currency_uuid, fetch_limit};
    std::string args = "$1, $2, $3, $4, $5";
    if (!cursor_date.empty()) {
        args += ", $6, $7";
        params.push_back(cursor_date);
        params.push_back(cursor_uuid);
    }
    append_filter_args(filter, args, params);
    db::Result result =
        db.exec(std::string("SELECT ") + kSelectColumns + " FROM f_operations_for_period(" + args + ")", params);

    OperationsResult out;
    std::size_t take = result.rows.size();
    if (take > static_cast<std::size_t>(limit)) {
        take = static_cast<std::size_t>(limit);
        out.has_more = true;
        // Курсор — последняя строка ИЗ ОТДАННЫХ (take-1), не подглядочная
        // (result.rows[limit]) — иначе следующая страница потеряла бы ровно
        // одну операцию (ту, что сейчас использована только как маркер
        // "есть ещё", но не показана пользователю).
        const auto& last = result.rows[take - 1].fields;
        out.next_cursor_date = last.at(1); // "date"
        out.next_cursor_uuid = last.at(0); // "operation_uuid"
    }
    for (std::size_t i = 0; i < take; ++i) {
        out.rows.push_back(row_to_json(result.rows[i].fields, kColumns));
        if (result.rows[i].fields.at(kAmountEqIdx).empty()) out.incomplete = true;
    }

    // Итог — вторым запросом и только при фильтрах (design D3): окно
    // sum() OVER () посчитало бы лишь строки до LIMIT, а нужен весь период.
    // Без фильтров запроса нет, и ответ прежний.
    if (filter.any()) {
        auto total = operations_total(db, ledger_id, from, to, eq_currency_uuid, filter);
        out.has_total = true;
        out.total_eq = total.total_eq;
        out.total_incomplete = total.incomplete;
        out.total_count = total.matched;
    }
    return out;
}

WalletTurnoverResult wallet_turnover_for_period(db::Database& db, const std::string& ledger_id,
                                                 const std::string& from, const std::string& to,
                                                 const std::string& eq_currency_uuid) {
    auto result = db.exec(
        "SELECT account_uuid, account_name, comment, is_archived, currency_uuid, symbol, "
        "\"precision\", icon_sha256, turnover_in, turnover_out, turnover_in_eq, turnover_out_eq, "
        "bool_or(incomplete) OVER () AS incomplete "
        "FROM f_wallet_turnover_for_period($1, $2, $3, $4)",
        {ledger_id, from, to, eq_currency_uuid});

    static const std::vector<std::string> kColumns = {
        "account_uuid", "account_name",   "comment",        "is_archived",     "currency_uuid",
        "symbol",       "precision",      "icon_sha256",    "turnover_in",     "turnover_out",
        "turnover_in_eq", "turnover_out_eq",
    };
    WalletTurnoverResult out;
    for (const auto& row : result.rows) {
        json out_row = row_to_json(row.fields, kColumns);
        // is_archived — единственная НЕ денежная/текстовая колонка здесь:
        // row_to_json кладёт все поля как есть строками (тот же приём, что и
        // у "precision" в остальных отчётах — см. BalanceReportRow), но
        // признак архивности клиенту удобнее получить настоящим JSON
        // boolean, а не строкой "t"/"f".
        out_row["is_archived"] = row.fields.at(3) == "t";
        out.rows.push_back(out_row);
        if (row.fields.at(kColumns.size()) == "t") out.incomplete = true;
    }
    return out;
}

BalancesAtDatesResult balances_at_dates(db::Database& db, const std::string& ledger_id,
                                         const std::vector<std::string>& dates,
                                         const std::string& eq_currency_uuid) {
    // Postgres array-литерал текстом — каждый элемент в кавычках: сама дата
    // не содержит зарезервированных символов массива ({},"\), но кавычки не
    // вредят и снимают вопрос про экранирование раз и навсегда. libpq шлёт
    // параметр без явного OID (см. db.hpp у exec()), Postgres кастует его по
    // объявленному типу параметра функции (timestamp[]) сам.
    std::string array_literal = "{";
    for (std::size_t i = 0; i < dates.size(); ++i) {
        if (i) array_literal += ",";
        array_literal += "\"" + dates[i] + "\"";
    }
    array_literal += "}";

    auto result = db.exec(
        "SELECT date_index, account_uuid, account_name, currency_uuid, symbol, \"precision\", "
        "balance, balance_eq, "
        "COALESCE(sum(balance_eq) OVER (PARTITION BY date_index), 0)::text AS total_eq, "
        "bool_or(balance_eq IS NULL) OVER (PARTITION BY date_index) AS incomplete "
        "FROM f_balance_to_dates($1, $2, $3)",
        {ledger_id, array_literal, eq_currency_uuid});

    static const std::vector<std::string> kColumns = {
        "account_uuid", "account_name", "currency_uuid", "symbol", "precision",
        "balance",      "balance_eq",
    };

    BalancesAtDatesResult out;
    // Ровно один блок на каждую dates[i], СРАЗУ, в исходном порядке — date
    // эхом повторяет строку запроса (не то, что вернул/нормализовал Postgres,
    // см. .hpp), а date_index (1-based, см. .sql — WITH ORDINALITY) даёт
    // индекс блока напрямую, без сопоставления по тексту даты.
    for (const auto& d : dates) {
        out.dates.push_back({{"date", d}, {"accounts", json::array()}, {"total_eq", "0"}, {"incomplete", false}});
    }
    for (const auto& row : result.rows) {
        std::size_t idx = static_cast<std::size_t>(std::stoul(row.fields.at(0))) - 1;
        if (idx >= out.dates.size()) continue; // защитно — не должно случиться, date_index всегда в пределах dates.size()
        json& block = out.dates.at(idx);
        std::vector<std::string> account_fields(row.fields.begin() + 1, row.fields.end() - 2);
        block["accounts"].push_back(row_to_json(account_fields, kColumns));
        block["total_eq"] = row.fields.at(8);
        block["incomplete"] = row.fields.at(9) == "t";
    }
    return out;
}

BalanceSeriesResult balance_series(db::Database& db, const std::string& ledger_id,
                                    const std::string& from, const std::string& to,
                                    const std::string& step, const std::string& eq_currency_uuid,
                                    bool with_accounts) {
    BalanceSeriesResult out;
    if (!with_accounts) {
        // Та же сумма и тот же incomplete, что и в ветке с accounts ниже,
        // только агрегированные прямо в базе — по одной строке на бакет
        // вместо строки на каждый счёт каждого бакета. Пустой бакет (одна
        // строка с account_uuid IS NULL) даёт sum = NULL -> COALESCE "0" и
        // bool_or = false, как и там.
        auto totals = db.exec(
            "SELECT bucket_start, bucket_end, COALESCE(sum(balance_eq), 0)::text, "
            "bool_or(account_uuid IS NOT NULL AND balance_eq IS NULL) "
            "FROM f_balance_series($1, $2, $3, $4, $5) "
            "GROUP BY date_index, bucket_start, bucket_end ORDER BY date_index",
            {ledger_id, from, to, step, eq_currency_uuid});
        for (const auto& row : totals.rows) {
            out.buckets.push_back({
                {"bucket_start", row.fields.at(0)},
                {"bucket_end", row.fields.at(1)},
                {"total_eq", row.fields.at(2)},
                {"incomplete", row.fields.at(3) == "t"},
            });
        }
        return out;
    }

    // incomplete — bool_or ТОЛЬКО по строкам с реальным account_uuid: пустой
    // бакет (account_uuid IS NULL, см. f_balance_series::LEFT JOIN buckets ->
    // acct_bal) сам по себе имеет balance_eq IS NULL, но это НЕ "курс не
    // нашёлся" — счетов там попросту нет, и считать такой бакет "неполным"
    // было бы неверно (тот же принцип, что и zero-fill incomplete у
    // f_income_expense_by_month, см. её README).
    auto result = db.exec(
        "SELECT date_index, bucket_start, bucket_end, account_uuid, account_name, currency_uuid, "
        "symbol, \"precision\", balance, balance_eq, "
        "COALESCE(sum(balance_eq) OVER (PARTITION BY date_index), 0)::text AS total_eq, "
        "bool_or(account_uuid IS NOT NULL AND balance_eq IS NULL) OVER (PARTITION BY date_index) AS incomplete "
        "FROM f_balance_series($1, $2, $3, $4, $5)",
        {ledger_id, from, to, step, eq_currency_uuid});

    static const std::vector<std::string> kColumns = {
        "account_uuid", "account_name", "currency_uuid", "symbol", "precision",
        "balance",      "balance_eq",
    };

    // f_balance_series отдаёт РОВНО одну строку на КАЖДЫЙ бакет (даже
    // пустой, см. её LEFT JOIN) плюс по одной строке на каждый счёт внутри
    // непустого — строго по возрастанию date_index (ORDER BY b.idx в самой
    // функции), поэтому новый блок заводится всякий раз, когда date_index
    // меняется, без отдельной map "индекс -> позиция блока".
    int last_idx = -1;
    for (const auto& row : result.rows) {
        int idx = std::stoi(row.fields.at(0));
        if (idx != last_idx) {
            out.buckets.push_back({
                {"bucket_start", row.fields.at(1)},
                {"bucket_end", row.fields.at(2)},
                {"accounts", json::array()},
                {"total_eq", row.fields.at(10)},
                {"incomplete", row.fields.at(11) == "t"},
            });
            last_idx = idx;
        }
        if (row.fields.at(3).empty()) continue; // пустой бакет — account_uuid пуст (SQL NULL), блок уже заведён выше
        std::vector<std::string> account_fields(row.fields.begin() + 3, row.fields.begin() + 10);
        out.buckets.back()["accounts"].push_back(row_to_json(account_fields, kColumns));
    }
    return out;
}

UserStatsResult list_user_stats(db::Database& db, const std::vector<std::string>& user_ids) {
    UserStatsResult out;
    if (user_ids.empty()) return out;

    // Только леджеры переданных пользователей и одним проходом по каждой
    // таблице: CTE owners сужает выборку до них, дальше по одному GROUP BY
    // на операции, кошельки и участников. Прежний вариант — три
    // коррелированных подзапроса НА КАЖДЫЙ леджер базы — считал всех
    // владельцев разом: на стенде с данными нагрузочного теста (4181 леджер
    // по 3000 операций) это 15,8 с и 1 МБ JSON, из которого таблица брала две
    // строки. Отдельные GROUP BY, а не один JOIN операций с кошельками, —
    // тот перемножил бы строки (кошельки × операции) на леджер.
    //
    // Список id — массивом в одном параметре ($1::uuid[]), литералом
    // Postgres '{a,b}': id уже проверены на формат UUID в reports.cpp, так что
    // ни запятых, ни скобок внутри быть не может.
    std::string ids_literal = "{";
    for (size_t i = 0; i < user_ids.size(); ++i) {
        if (i) ids_literal += ',';
        ids_literal += user_ids[i];
    }
    ids_literal += '}';

    // a.kind = 1 — явный признак "это кошелёк, не категория" (см.
    // wallet-api/schema/table/account.sql).
    auto res = db.exec(
        "WITH owners AS ("
        "  SELECT user_id, ledger_id FROM ledger_member"
        "   WHERE role = 'owner' AND user_id = ANY($1::uuid[])), "
        "ops AS ("
        "  SELECT o.ledger_id, count(*) AS n, min(o.date) AS first_date, max(o.date) AS last_date"
        "    FROM operation o JOIN owners ow ON ow.ledger_id = o.ledger_id"
        "   WHERE o.deleted_at IS NULL GROUP BY o.ledger_id), "
        "wallets AS ("
        "  SELECT a.ledger_id, count(*) AS n"
        "    FROM account a JOIN owners ow ON ow.ledger_id = a.ledger_id"
        "   WHERE a.kind = 1 AND a.deleted_at IS NULL GROUP BY a.ledger_id), "
        "members AS ("
        "  SELECT m.ledger_id, count(*) AS n"
        "    FROM ledger_member m JOIN owners ow ON ow.ledger_id = m.ledger_id GROUP BY m.ledger_id) "
        "SELECT ow.user_id, ow.ledger_id, coalesce(w.n, 0), coalesce(op.n, 0),"
        "       op.first_date, op.last_date, coalesce(mb.n, 0) "
        "  FROM owners ow"
        "  LEFT JOIN ops op ON op.ledger_id = ow.ledger_id"
        "  LEFT JOIN wallets w ON w.ledger_id = ow.ledger_id"
        "  LEFT JOIN members mb ON mb.ledger_id = ow.ledger_id "
        " ORDER BY ow.user_id",
        {ids_literal});

    static const std::vector<std::string> kColumns = {
        "user_id", "ledger_id", "wallet_count", "operation_count",
        "first_operation_date", "last_operation_date", "member_count",
    };

    for (const auto& row : res.rows) out.users.push_back(row_to_json(row.fields, kColumns));
    return out;
}

IconsResult list_icons(db::Database& db, const std::optional<std::string>& ledger_id) {
    static const std::vector<std::string> kColumns = {"uuid", "name", "icon_type", "sha256"};

    IconsResult result;
    auto common_res = db.exec(
        "SELECT uuid, name, icon_type, encode(content_sha256, 'hex') FROM icon "
        "WHERE ledger_id IS NULL AND deleted_at IS NULL ORDER BY name");
    for (const auto& row : common_res.rows) result.common.push_back(row_to_json(row.fields, kColumns));

    if (ledger_id) {
        auto user_res = db.exec(
            "SELECT uuid, name, icon_type, encode(content_sha256, 'hex') FROM icon "
            "WHERE ledger_id = $1 AND deleted_at IS NULL ORDER BY name",
            {*ledger_id});
        for (const auto& row : user_res.rows) result.user.push_back(row_to_json(row.fields, kColumns));
    }

    return result;
}

} // namespace reports_repo
