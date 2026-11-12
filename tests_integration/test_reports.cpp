// Интеграционные тесты reports_repo.hpp (перенесены из wallet-api при
// выделении этого сервиса, org/docs/TODO_mobile_sync.md, Фаза 4) — против
// ЖИВОГО Postgres, ролью wallet_reader (см. fixture::open_reports, зачем
// именно этой ролью, а не той же, что пишет данные). Данные сеются через
// sync_repo::push_batch() — уже проверенный в wallet-api путь (см. его
// test_push_batch.cpp), а не руками через SQL: заодно лишний раз
// подтверждает, что отчёты читают данные ТАК ЖЕ, как их видит протокол
// синка, не какой-то отдельный, рассинхронизировавшийся способ записи.
// sync_repo.cpp/provisioning.cpp здесь — ТОЛЬКО ради этой сеялки (см.
// CMakeLists.txt рядом) — сам сервис wallet_report их не содержит вовсе.
#include "reports_repo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "fixture.hpp"
#include "nlohmann/json.hpp"
#include "sync_repo.hpp"
#include "test_framework.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace {

json currency_row(const std::string& uuid, const std::string& icon_uuid, const std::string& iso_code) {
    return json{{"uuid", uuid},        {"name", iso_code},      {"iso_code", iso_code},
                {"icon_uuid", icon_uuid}, {"symbol", iso_code}, {"precision", 2}};
}

// system_kind/kind — 0 везде в этом файле: ни один тест здесь не проверяет
// ни Transfer/Transfer commission, ни разделение кошелёк/категория
// (обсуждение 2026-09-09, см. wallet-api/schema/table/account.sql) — sync_repo
// требует оба поля в push_batch (TableSpec account), не только конкретное
// значение.
json account_row(const std::string& uuid, const std::string& icon_uuid, const std::string& name) {
    return json{{"uuid", uuid}, {"name", name}, {"comment", ""}, {"icon_uuid", icon_uuid},
                {"is_archived", false}, {"system_kind", 0}, {"kind", 0}};
}

// uuid ЭТОЙ строки = account.uuid того же счёта (инвариант, см.
// wallet-api/schema/table/account_operating.sql) — присутствие такой строки и
// делает счёт "кошельком" (operating), а не категорией, см. f_income_for_period.sql.
json account_operating_row(const std::string& account_uuid, const std::string& currency_uuid) {
    return json{{"uuid", account_uuid}, {"currency_uuid", currency_uuid}};
}

json operation_row(const std::string& uuid, const std::string& date, const std::string& from_uuid,
                    const std::string& to_uuid, const std::string& amount) {
    return json{{"uuid", uuid},       {"date", date},           {"acc_fr_uuid", from_uuid},
                {"acc_to_uuid", to_uuid}, {"amount", amount}, {"comment", "reports test"}};
}

// base -> quote направление (v_currency_conversion, ветка is_reverse=false) —
// currency_pair_rate ниже задаёт курс ИМЕННО base->quote напрямую.
json currency_pair_row(const std::string& uuid, const std::string& symbol,
                        const std::string& base_uuid, const std::string& quote_uuid) {
    return json{{"uuid", uuid},
                {"symbol", symbol},
                {"base_currency_uuid", base_uuid},
                {"quote_currency_uuid", quote_uuid},
                {"precision", 4}};
}

json currency_pair_rate_row(const std::string& uuid, const std::string& pair_uuid, const std::string& date,
                             const std::string& rate) {
    return json{{"uuid", uuid}, {"currency_pair_uuid", pair_uuid}, {"date", date}, {"rate", rate}};
}

// type: 0 = нога валютного перевода, 1 = комиссия (OperationItem.LinkType на клиенте,
// см. wallet-api/schema/table/operation_link.sql).
json operation_link_row(const std::string& uuid, const std::string& main_uuid, const std::string& link_uuid,
                         int type) {
    return json{{"uuid", uuid}, {"main_uuid", main_uuid}, {"link_uuid", link_uuid}, {"type", type}};
}

} // namespace

TEST(find_ledger_for_user_returns_nullopt_for_unknown_user) {
    auto reports_db = fixture::open_reports();
    auto result = reports_repo::find_ledger_for_user(*reports_db, uuidgen::v4());
    CHECK(!result.has_value());
}

TEST(resolve_eq_currency_returns_nullopt_for_unknown_iso_code) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto result = reports_repo::resolve_eq_currency(*reports_db, "ZZZ" + uuidgen::v4().substr(0, 8),
                                                     identity.ledger_id);
    CHECK(!result.has_value());
}

TEST(balance_to_date_reflects_seeded_operation) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string acc_fr = uuidgen::v4();
    std::string acc_to = uuidgen::v4();
    std::string op_uuid = uuidgen::v4();
    std::string iso = "US" + uuidgen::v4().substr(0, 1); // уникальный ISO-код на тест — таблица общая между тестами

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(acc_fr, icon_uuid, "From " + acc_fr),
                                  account_row(acc_to, icon_uuid, "To " + acc_to)})},
        {"account_operating", json::array({account_operating_row(acc_fr, currency_uuid),
                                            account_operating_row(acc_to, currency_uuid)})},
        {"operation",
         json::array({operation_row(op_uuid, "2026-01-15T10:00:00", acc_fr, acc_to, "500.00")})},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto ledger_id = reports_repo::find_ledger_for_user(*reports_db, identity.user_id);
    REQUIRE(ledger_id.has_value());
    CHECK_EQ(*ledger_id, identity.ledger_id);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());
    CHECK_EQ(*eq_currency, currency_uuid);

    // Дата ПОСЛЕ операции — f_balance_to_date считает строго ДО p_date (см.
    // её комментарий), значит операция должна попасть в баланс.
    auto result =
        reports_repo::balance_to_date(*reports_db, identity.ledger_id, "2026-12-31", *eq_currency);

    REQUIRE(result.rows.size() == static_cast<size_t>(2));
    bool found_to = false;
    bool found_fr = false;
    for (const auto& row : result.rows) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        if (uuid == acc_to) {
            CHECK_EQ(row.at("balance").get<std::string>(), std::string("500.00"));
            // Курс на саму себя — 1 (см. v_currency_conversion_rate.sql, UNION на
            // каждую валюту), поэтому balance_eq совпадает с balance дословно.
            CHECK_EQ(row.at("balance_eq").get<std::string>(), std::string("500.00"));
            found_to = true;
        }
        if (uuid == acc_fr) {
            CHECK_EQ(row.at("balance").get<std::string>(), std::string("-500.00"));
            found_fr = true;
        }
    }
    CHECK(found_to);
    CHECK(found_fr);
    CHECK(!result.incomplete); // курс на саму себя есть всегда
    // +500 и -500 в сумме дают ровно 0 — та самая сверка, о которой
    // предупреждает комментарий в f_balance_to_date.sql (не проверяем на
    // NULL/произвольное значение, а на конкретный, предсказуемый итог).
    CHECK_EQ(result.total_eq, std::string("0.00"));
}

TEST(income_for_period_reflects_seeded_operation) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string category_uuid = uuidgen::v4(); // без account_operating — категория
    std::string wallet_uuid = uuidgen::v4();   // с account_operating — кошелёк
    std::string op_uuid = uuidgen::v4();
    std::string iso = "EU" + uuidgen::v4().substr(0, 1);

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Salary " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        // ИЗ категории В кошелёк — ровно направление, которое f_income_for_period
        // считает доходом (см. её комментарий: "доход от расхода отличает
        // направление, а не знак суммы").
        {"operation", json::array({operation_row(op_uuid, "2026-03-10T09:00:00", category_uuid,
                                                  wallet_uuid, "1000.00")})},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::income_for_period(
        *reports_db, identity.ledger_id, "2026-03-01T00:00:00", "2026-03-31T23:59:59", *eq_currency);

    REQUIRE(result.rows.size() == static_cast<size_t>(1));
    CHECK_EQ(result.rows[0].at("account_uuid").get<std::string>(), category_uuid);
    CHECK_EQ(result.rows[0].at("amount").get<std::string>(), std::string("1000.00"));
    CHECK_EQ(result.total_eq, std::string("1000.00"));
    CHECK(!result.incomplete);

    // Та же операция НЕ должна попасть в expense (направление обратное) —
    // защитная проверка, что period_report() не перепутал местами
    // acc_from_is_operating/acc_to_is_operating между двумя функциями.
    auto expense = reports_repo::expense_for_period(
        *reports_db, identity.ledger_id, "2026-03-01T00:00:00", "2026-03-31T23:59:59", *eq_currency);
    CHECK_EQ(expense.rows.size(), static_cast<size_t>(0));
}

// Валютный перевод раньше раздувал и «Доход», и «Расход» одновременно (обсуждение
// 2026-09-09, android-wallet/docs/TODO_transfers.md, п.3): debit-нога (кошелёк → Transfer)
// считалась полноценным расходом, credit-нога (Transfer → кошелёк) — полноценным
// доходом. Обе ноги должны быть исключены из своих гросс-сумм, а их net (курсовая
// разница между двумя валютами перевода) — попасть ОДНОЙ строкой на счёте Transfer
// в тот список, куда указывает его знак.
TEST(income_and_expense_for_period_net_cross_currency_transfer_legs) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string usd = uuidgen::v4();
    std::string eur = uuidgen::v4();
    std::string iso_usd = "TU" + uuidgen::v4().substr(0, 1);
    std::string iso_eur = "TE" + uuidgen::v4().substr(0, 1);
    std::string pair_uuid = uuidgen::v4();

    std::string wallet_a = uuidgen::v4();    // USD
    std::string wallet_b = uuidgen::v4();    // EUR
    std::string transfer_uuid = uuidgen::v4();
    std::string category_uuid = uuidgen::v4(); // обычная категория рядом — контроль, что её не задело

    std::string op_main = uuidgen::v4();    // wallet_a(USD) -> Transfer, 100.00
    std::string op_leg = uuidgen::v4();     // Transfer -> wallet_b(EUR), 92.00 (курс 1.10 = 101.20 USD)
    std::string op_expense = uuidgen::v4(); // обычный расход рядом, не должен пострадать

    // system_kind=1 (TRANSFER) — единственное поле этого счёта, отличное от
    // account_row(); f_income_for_period/f_expanse_for_period находят счёт для
    // net-строки именно по нему, а не по имени.
    json transfer_row = json{{"uuid", transfer_uuid},   {"name", "Transfer " + transfer_uuid},
                              {"comment", ""},           {"icon_uuid", icon_uuid},
                              {"is_archived", false},    {"system_kind", 1},
                              {"kind", 0}};

    json changes = {
        {"currency", json::array({currency_row(usd, icon_uuid, iso_usd),
                                   currency_row(eur, icon_uuid, iso_eur)})},
        {"currency_pair", json::array({currency_pair_row(pair_uuid, iso_eur + iso_usd, eur, usd)})},
        {"currency_pair_rate",
         json::array({currency_pair_rate_row(uuidgen::v4(), pair_uuid, "2026-05-01", "1.10")})},
        {"account", json::array({account_row(wallet_a, icon_uuid, "Wallet A " + wallet_a),
                                  account_row(wallet_b, icon_uuid, "Wallet B " + wallet_b),
                                  account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  transfer_row})},
        {"account_operating", json::array({account_operating_row(wallet_a, usd),
                                            account_operating_row(wallet_b, eur)})},
        {"operation", json::array({
             operation_row(op_main, "2026-05-15T10:00:00", wallet_a, transfer_uuid, "100.00"),
             operation_row(op_leg, "2026-05-15T10:00:00", transfer_uuid, wallet_b, "92.00"),
             operation_row(op_expense, "2026-05-16T10:00:00", wallet_a, category_uuid, "30.00"),
         })},
        {"operation_link", json::array({operation_link_row(uuidgen::v4(), op_main, op_leg, 0)})},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso_usd, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto income = reports_repo::income_for_period(
        *reports_db, identity.ledger_id, "2026-05-01T00:00:00", "2026-05-31T23:59:59", *eq_currency);
    auto expense = reports_repo::expense_for_period(
        *reports_db, identity.ledger_id, "2026-05-01T00:00:00", "2026-05-31T23:59:59", *eq_currency);

    // net = credit(92.00 EUR * 1.10) - debit(100.00 USD) = 101.20 - 100.00 = +1.20 —
    // положительный, значит ОДНА строка на Transfer в income, и ничего от перевода в expense.
    REQUIRE(income.rows.size() == static_cast<size_t>(1));
    CHECK_EQ(income.rows[0].at("account_uuid").get<std::string>(), transfer_uuid);
    CHECK_EQ(income.rows[0].at("amount").get<std::string>(), std::string("1.20"));
    CHECK_EQ(income.rows[0].at("amount_eq").get<std::string>(), std::string("1.20"));

    // Обычный расход рядом остался нетронутым, а ноги перевода в expense не попали
    // вовсе — net положительный, ушёл в income.
    REQUIRE(expense.rows.size() == static_cast<size_t>(1));
    CHECK_EQ(expense.rows[0].at("account_uuid").get<std::string>(), category_uuid);
    CHECK_EQ(expense.rows[0].at("amount").get<std::string>(), std::string("30.00"));
}

// "Структура расходов" (servers/Design/.../Отчёты - веб.dc.html) — категория,
// оплаченная в двух РАЗНЫХ валютах кошелька, обязана стать ОДНОЙ строкой
// (см. f_expanse_by_category_for_period.sql), в отличие от f_expanse_for_period,
// где та же ситуация закономерно даёт две строки (группировка ещё и по
// currency_uuid) — тест проверяет ОБА факта разом, чтобы регрессия на любой
// из функций была видна.
TEST(expense_by_category_for_period_sums_across_currencies_unlike_expense_for_period) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string icon_uuid = icon.uuid;
    std::string cur_a = uuidgen::v4(); // валюта эквивалента и валюта wallet_a
    std::string cur_b = uuidgen::v4(); // валюта wallet_b — курс к cur_a через пару
    std::string iso_a = "AA" + uuidgen::v4().substr(0, 1);
    std::string iso_b = "BB" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_a = uuidgen::v4();
    std::string wallet_b = uuidgen::v4();
    std::string pair_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(cur_a, icon_uuid, iso_a), currency_row(cur_b, icon_uuid, iso_b)})},
        {"currency_pair", json::array({currency_pair_row(pair_uuid, iso_b + "/" + iso_a, cur_b, cur_a)})},
        // base(cur_b) -> quote(cur_a) = 2, курс задан ДО обеих операций.
        {"currency_pair_rate",
         json::array({currency_pair_rate_row(uuidgen::v4(), pair_uuid, "2026-04-01", "2.0000")})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_a, icon_uuid, "Wallet A " + wallet_a),
                                  account_row(wallet_b, icon_uuid, "Wallet B " + wallet_b)})},
        {"account_operating", json::array({account_operating_row(wallet_a, cur_a),
                                            account_operating_row(wallet_b, cur_b)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-04-10T09:00:00", wallet_a, category_uuid, "100.00"),
             // 50.00 * курс 2 = 100.00 в эквиваленте — тот же вклад, что и
             // операция выше, специально, чтобы итог 200.00 было легко
             // проверить на глаз.
             operation_row(uuidgen::v4(), "2026-04-11T09:00:00", wallet_b, category_uuid, "50.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso_a, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    // Базовый отчёт (не структура) — две строки, разная currency_uuid у
    // каждой (регрессия здесь означала бы, что f_expanse_for_period сам
    // начал схлопывать валюты — не должен).
    auto flat = reports_repo::expense_for_period(*reports_db, identity.ledger_id, "2026-04-01T00:00:00",
                                                  "2026-04-30T23:59:59", *eq_currency);
    REQUIRE(flat.rows.size() == static_cast<size_t>(2));

    // Структурный отчёт — ОДНА строка на категорию, сумма обеих валют в
    // эквиваленте сложена.
    auto by_cat = reports_repo::expense_by_category_for_period(
        *reports_db, identity.ledger_id, "2026-04-01T00:00:00", "2026-04-30T23:59:59", *eq_currency);
    REQUIRE(by_cat.rows.size() == static_cast<size_t>(1));
    CHECK_EQ(by_cat.rows[0].at("account_uuid").get<std::string>(), category_uuid);
    // Округление — по precision ВАЛЮТЫ ЭКВИВАЛЕНТА (cur_a, precision 2 —
    // см. currency_row), не по precision пары (4 у currency_pair выше, он
    // вообще не участвует в округлении amount_eq).
    CHECK_EQ(by_cat.rows[0].at("amount_eq").get<std::string>(), std::string("200.00"));
    CHECK_EQ(by_cat.total_eq, std::string("200.00"));
    CHECK(!by_cat.incomplete);
    // icon_sha256 (000005_expense_structure_icon) — category_uuid заведена
    // с этой же фикстурной иконкой, sha256 должен совпасть буквально.
    CHECK_EQ(by_cat.rows[0].at("icon_sha256").get<std::string>(), icon.sha256_hex);
}

// Зеркало income_and_expense_for_period_net_cross_currency_transfer_legs выше,
// но для f_expanse_by_category_for_period (000022_transfer_net_in_category_structure)
// — "Структура расходов" до этой миграции считала обе ноги перевода гроссом на
// категории Transfer, не исключая их и не сворачивая в net, в отличие от
// f_expanse_for_period. Здесь net отрицательный, значит ОДНА строка на Transfer
// должна попасть в expense_by_category_for_period с суммой |net|, а обычная
// категория рядом — остаться нетронутой.
TEST(expense_by_category_for_period_nets_cross_currency_transfer_legs) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string icon_uuid = icon.uuid;
    std::string usd = uuidgen::v4();
    std::string eur = uuidgen::v4();
    std::string iso_usd = "NU" + uuidgen::v4().substr(0, 1);
    std::string iso_eur = "NE" + uuidgen::v4().substr(0, 1);
    std::string pair_uuid = uuidgen::v4();

    std::string wallet_a = uuidgen::v4();    // USD
    std::string wallet_b = uuidgen::v4();    // EUR
    std::string transfer_uuid = uuidgen::v4();
    std::string category_uuid = uuidgen::v4(); // обычная категория рядом — контроль, что её не задело

    std::string op_main = uuidgen::v4();    // wallet_a(USD) -> Transfer, 100.00
    std::string op_leg = uuidgen::v4();     // Transfer -> wallet_b(EUR), 80.00 (курс 1.10 = 88.00 USD)
    std::string op_expense = uuidgen::v4(); // обычный расход рядом, не должен пострадать

    json transfer_row = json{{"uuid", transfer_uuid},   {"name", "Transfer " + transfer_uuid},
                              {"comment", ""},           {"icon_uuid", icon_uuid},
                              {"is_archived", false},    {"system_kind", 1},
                              {"kind", 0}};

    json changes = {
        {"currency", json::array({currency_row(usd, icon_uuid, iso_usd),
                                   currency_row(eur, icon_uuid, iso_eur)})},
        {"currency_pair", json::array({currency_pair_row(pair_uuid, iso_eur + iso_usd, eur, usd)})},
        {"currency_pair_rate",
         json::array({currency_pair_rate_row(uuidgen::v4(), pair_uuid, "2026-06-01", "1.10")})},
        {"account", json::array({account_row(wallet_a, icon_uuid, "Wallet A " + wallet_a),
                                  account_row(wallet_b, icon_uuid, "Wallet B " + wallet_b),
                                  account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  transfer_row})},
        {"account_operating", json::array({account_operating_row(wallet_a, usd),
                                            account_operating_row(wallet_b, eur)})},
        {"operation", json::array({
             operation_row(op_main, "2026-06-15T10:00:00", wallet_a, transfer_uuid, "100.00"),
             operation_row(op_leg, "2026-06-15T10:00:00", transfer_uuid, wallet_b, "80.00"),
             operation_row(op_expense, "2026-06-16T10:00:00", wallet_a, category_uuid, "30.00"),
         })},
        {"operation_link", json::array({operation_link_row(uuidgen::v4(), op_main, op_leg, 0)})},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso_usd, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto by_cat = reports_repo::expense_by_category_for_period(
        *reports_db, identity.ledger_id, "2026-06-01T00:00:00", "2026-06-30T23:59:59", *eq_currency);

    // net = credit(80.00 EUR * 1.10) - debit(100.00 USD) = 88.00 - 100.00 = -12.00 —
    // отрицательный, значит ОДНА строка на Transfer с суммой 12.00, гросс-ноги
    // (100.00 и 88.00) в неё не попадают.
    REQUIRE(by_cat.rows.size() == static_cast<size_t>(2));
    bool found_transfer = false, found_category = false;
    for (const auto& row : by_cat.rows) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        if (uuid == transfer_uuid) {
            CHECK_EQ(row.at("amount_eq").get<std::string>(), std::string("12.00"));
            CHECK_EQ(row.at("icon_sha256").get<std::string>(), icon.sha256_hex);
            found_transfer = true;
        } else if (uuid == category_uuid) {
            CHECK_EQ(row.at("amount_eq").get<std::string>(), std::string("30.00"));
            found_category = true;
        }
    }
    CHECK(found_transfer);
    CHECK(found_category);
    CHECK_EQ(by_cat.total_eq, std::string("42.00"));
}

TEST(expense_structure_for_period_computes_pct_without_comparison) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string icon_uuid = icon.uuid;
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "PC" + uuidgen::v4().substr(0, 1);
    std::string wallet_uuid = uuidgen::v4();
    std::string cat_big = uuidgen::v4();
    std::string cat_small = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid),
                                  account_row(cat_big, icon_uuid, "Big " + cat_big),
                                  account_row(cat_small, icon_uuid, "Small " + cat_small)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-05-05T10:00:00", wallet_uuid, cat_big, "300.00"),
             operation_row(uuidgen::v4(), "2026-05-06T10:00:00", wallet_uuid, cat_small, "100.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::expense_structure_for_period(
        *reports_db, identity.ledger_id, "2026-05-01T00:00:00", "2026-05-31T23:59:59", *eq_currency,
        std::nullopt, std::nullopt);

    CHECK(!result.compared);
    CHECK_EQ(result.total_eq, std::string("400.00"));
    REQUIRE(result.categories.size() == static_cast<size_t>(2));

    bool found_big = false, found_small = false;
    for (const auto& row : result.categories) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        // Без периода сравнения pct_before/pp_delta не добавляются вовсе —
        // не JSON null, а просто отсутствующий ключ (см. reports.cpp: тело
        // ответа собирается из categories как есть).
        CHECK(!row.contains("pct_before"));
        CHECK(!row.contains("pp_delta"));
        // icon_sha256 — hex content_sha256 РОВНО этой иконки (000005_expense_structure_icon),
        // не просто "выглядит как hex": обе категории здесь заведены с одной
        // и той же фикстурной иконкой (icon_uuid), значит и sha256 у обеих
        // должен совпасть с тем, что вернула сама фикстура.
        CHECK_EQ(row.at("icon_sha256").get<std::string>(), icon.sha256_hex);
        if (uuid == cat_big) {
            CHECK_EQ(row.at("pct").get<std::string>(), std::string("75.0"));
            found_big = true;
        }
        if (uuid == cat_small) {
            CHECK_EQ(row.at("pct").get<std::string>(), std::string("25.0"));
            found_small = true;
        }
    }
    CHECK(found_big);
    CHECK(found_small);
}

// Сравнение периодов: категория с ростом доли, и категория, которая в
// текущем периоде вовсе исчезла (была только в cmp-периоде) — обе обязаны
// попасть в ответ, см. reports_repo.hpp у StructureResult.
TEST(expense_structure_for_period_with_comparison_reports_pp_delta_and_dropped_category) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string icon_uuid = icon.uuid;
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "CM" + uuidgen::v4().substr(0, 1);
    std::string wallet_uuid = uuidgen::v4();
    std::string cat_stayed = uuidgen::v4();
    std::string cat_dropped = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid),
                                  account_row(cat_stayed, icon_uuid, "Stayed " + cat_stayed),
                                  account_row(cat_dropped, icon_uuid, "Dropped " + cat_dropped)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             // "До" (март) — обе категории, поровну (50/50).
             operation_row(uuidgen::v4(), "2026-03-05T10:00:00", wallet_uuid, cat_stayed, "50.00"),
             operation_row(uuidgen::v4(), "2026-03-06T10:00:00", wallet_uuid, cat_dropped, "50.00"),
             // "Сейчас" (апрель) — только cat_stayed, вся сумма в неё.
             operation_row(uuidgen::v4(), "2026-04-05T10:00:00", wallet_uuid, cat_stayed, "100.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::expense_structure_for_period(
        *reports_db, identity.ledger_id, "2026-04-01T00:00:00", "2026-04-30T23:59:59", *eq_currency,
        std::optional<std::string>("2026-03-01T00:00:00"), std::optional<std::string>("2026-03-31T23:59:59"));

    CHECK(result.compared);
    CHECK_EQ(result.total_eq, std::string("100.00"));
    CHECK_EQ(result.total_eq_before, std::string("100.00"));
    REQUIRE(result.categories.size() == static_cast<size_t>(2));

    bool found_stayed = false, found_dropped = false;
    for (const auto& row : result.categories) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        if (uuid == cat_stayed) {
            CHECK_EQ(row.at("amount_eq").get<std::string>(), std::string("100.00"));
            CHECK_EQ(row.at("pct").get<std::string>(), std::string("100.0"));
            CHECK_EQ(row.at("pct_before").get<std::string>(), std::string("50.0"));
            CHECK_EQ(row.at("pp_delta").get<std::string>(), std::string("50.0"));
            found_stayed = true;
        }
        if (uuid == cat_dropped) {
            CHECK_EQ(row.at("amount_eq").get<std::string>(), std::string("0"));
            CHECK_EQ(row.at("pct").get<std::string>(), std::string("0.0"));
            CHECK_EQ(row.at("pct_before").get<std::string>(), std::string("50.0"));
            CHECK_EQ(row.at("pp_delta").get<std::string>(), std::string("-50.0"));
            // Регрессия на именно эту ветку (категория есть только в
            // cmp-периоде) — иконка должна пережить перенос через
            // before_row_by_uuid в expense_structure_for_period(), не
            // потеряться при сборке "синтетической" строки.
            CHECK_EQ(row.at("icon_sha256").get<std::string>(), icon.sha256_hex);
            found_dropped = true;
        }
    }
    CHECK(found_stayed);
    CHECK(found_dropped);
}

// "Обзор" (000006_overview) — три теста ниже, по одному на новую функцию.

TEST(income_expense_by_month_zero_fills_months_without_activity) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "MO" + uuidgen::v4().substr(0, 1);
    std::string wallet_uuid = uuidgen::v4();
    std::string category_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid),
                                  account_row(category_uuid, icon_uuid, "Cat " + category_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             // Январь — расход (кошелёк -> категория).
             operation_row(uuidgen::v4(), "2026-01-15T10:00:00", wallet_uuid, category_uuid, "100.00"),
             // Февраль — намеренно пусто, должен зануглиться, не пропасть.
             // Март — доход (категория -> кошелёк).
             operation_row(uuidgen::v4(), "2026-03-10T10:00:00", category_uuid, wallet_uuid, "50.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::income_expense_by_month(
        *reports_db, identity.ledger_id, "2026-01-01T00:00:00", "2026-03-31T23:59:59", *eq_currency);

    REQUIRE(result.months.size() == static_cast<size_t>(3)); // янв/фев/март — ровно 3, не 2 (фев без операций)
    CHECK(!result.incomplete);

    CHECK_EQ(result.months[0].at("month_start").get<std::string>(), std::string("2026-01-01"));
    CHECK_EQ(result.months[0].at("expense_eq").get<std::string>(), std::string("100.00"));
    CHECK_EQ(result.months[0].at("income_eq").get<std::string>(), std::string("0.00"));

    CHECK_EQ(result.months[1].at("month_start").get<std::string>(), std::string("2026-02-01"));
    CHECK_EQ(result.months[1].at("expense_eq").get<std::string>(), std::string("0.00"));
    CHECK_EQ(result.months[1].at("income_eq").get<std::string>(), std::string("0.00"));

    CHECK_EQ(result.months[2].at("month_start").get<std::string>(), std::string("2026-03-01"));
    CHECK_EQ(result.months[2].at("expense_eq").get<std::string>(), std::string("0.00"));
    CHECK_EQ(result.months[2].at("income_eq").get<std::string>(), std::string("50.00"));
}

TEST(expense_by_weekday_zero_fills_days_without_expense) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "WD" + uuidgen::v4().substr(0, 1);
    std::string wallet_uuid = uuidgen::v4();
    std::string category_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid),
                                  account_row(category_uuid, icon_uuid, "Cat " + category_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-04-08T10:00:00", wallet_uuid, category_uuid, "70.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::expense_by_weekday(*reports_db, identity.ledger_id,
                                                    "2026-04-01T00:00:00", "2026-04-30T23:59:59",
                                                    *eq_currency);

    REQUIRE(result.days.size() == static_cast<size_t>(7)); // isodow 1..7, все семь, не только те, где были траты
    CHECK(!result.incomplete);

    // Не привязываемся к КОНКРЕТНОМУ isodow 2026-04-08 (не хотим дублировать
    // календарную арифметику в тесте) — проверяем структурно: ровно один
    // день с ненулевой суммой, она равна сумме операции, остальные шесть — 0.00.
    int nonzero_count = 0;
    std::string nonzero_amount;
    for (const auto& day : result.days) {
        std::string expense = day.at("expense_eq").get<std::string>();
        if (expense != "0.00") {
            nonzero_count++;
            nonzero_amount = expense;
        }
    }
    CHECK_EQ(nonzero_count, 1);
    CHECK_EQ(nonzero_amount, std::string("70.00"));
}

TEST(expense_biggest_moves_ranks_by_absolute_delta_desc_and_respects_limit) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "BM" + uuidgen::v4().substr(0, 1);
    std::string wallet_uuid = uuidgen::v4();
    std::string cat_grew_more = uuidgen::v4();  // |500-50| = 450 — самая большая дельта
    std::string cat_stayed = uuidgen::v4();     // |300-100| = 200
    std::string cat_dropped = uuidgen::v4();    // |0-80| = 80 — меньше limit=2, не должна попасть

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid),
                                  account_row(cat_grew_more, icon_uuid, "Grew " + cat_grew_more),
                                  account_row(cat_stayed, icon_uuid, "Stayed " + cat_stayed),
                                  account_row(cat_dropped, icon_uuid, "Dropped " + cat_dropped)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             // "До" (март).
             operation_row(uuidgen::v4(), "2026-03-05T10:00:00", wallet_uuid, cat_grew_more, "50.00"),
             operation_row(uuidgen::v4(), "2026-03-06T10:00:00", wallet_uuid, cat_stayed, "100.00"),
             operation_row(uuidgen::v4(), "2026-03-07T10:00:00", wallet_uuid, cat_dropped, "80.00"),
             // "Сейчас" (апрель) — cat_dropped вообще не встречается.
             operation_row(uuidgen::v4(), "2026-04-05T10:00:00", wallet_uuid, cat_grew_more, "500.00"),
             operation_row(uuidgen::v4(), "2026-04-06T10:00:00", wallet_uuid, cat_stayed, "300.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::expense_biggest_moves(
        *reports_db, identity.ledger_id, "2026-04-01T00:00:00", "2026-04-30T23:59:59",
        "2026-03-01T00:00:00", "2026-03-31T23:59:59", *eq_currency, 2);

    REQUIRE(result.rows.size() == static_cast<size_t>(2)); // limit=2 — cat_dropped (|Δ|=80, меньше обоих) не попал
    CHECK(!result.incomplete);

    CHECK_EQ(result.rows[0].at("account_uuid").get<std::string>(), cat_grew_more);
    CHECK_EQ(result.rows[0].at("amount_now_eq").get<std::string>(), std::string("500.00"));
    CHECK_EQ(result.rows[0].at("amount_before_eq").get<std::string>(), std::string("50.00"));

    CHECK_EQ(result.rows[1].at("account_uuid").get<std::string>(), cat_stayed);
    CHECK_EQ(result.rows[1].at("amount_now_eq").get<std::string>(), std::string("300.00"));
    CHECK_EQ(result.rows[1].at("amount_before_eq").get<std::string>(), std::string("100.00"));
}

// "Список операций" (000007_operations) — три проводки, по одной на каждый
// kind, проверяют разом: правильную классификацию по acc_from/acc_to_is_operating,
// заполнение category/wallet у expense/income и их ОТСУТСТВИЕ (пустая строка,
// SQL NULL — см. db.hpp) у transfer, и что у transfer вместо этого
// заполнены from_uuid/to_uuid ОБОИХ кошельков.
TEST(operations_for_period_classifies_kind_and_fills_category_or_wallet_by_side) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string icon_uuid = icon.uuid;
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "OP" + uuidgen::v4().substr(0, 1);
    std::string wallet_a = uuidgen::v4();
    std::string wallet_b = uuidgen::v4();
    std::string category_uuid = uuidgen::v4();
    std::string op_expense = uuidgen::v4();
    std::string op_income = uuidgen::v4();
    std::string op_transfer = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(wallet_a, icon_uuid, "Wallet A " + wallet_a),
                                  account_row(wallet_b, icon_uuid, "Wallet B " + wallet_b),
                                  account_row(category_uuid, icon_uuid, "Cat " + category_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_a, currency_uuid),
                                            account_operating_row(wallet_b, currency_uuid)})},
        {"operation", json::array({
             // Расход: кошелёк -> категория.
             operation_row(op_expense, "2026-06-05T10:00:00", wallet_a, category_uuid, "40.00"),
             // Доход: категория -> кошелёк.
             operation_row(op_income, "2026-06-06T10:00:00", category_uuid, wallet_a, "25.00"),
             // Перевод: кошелёк -> кошелёк, обе стороны operating.
             operation_row(op_transfer, "2026-06-07T10:00:00", wallet_a, wallet_b, "10.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::operations_for_period(
        *reports_db, identity.ledger_id, "2026-06-01T00:00:00", "2026-06-30T23:59:59", *eq_currency,
        1000, "", ""); // limit с запасом, без курсора — вся страница целиком, в этом тесте пагинация не при делах

    REQUIRE(result.rows.size() == static_cast<size_t>(3));
    CHECK(!result.incomplete); // курс на саму себя есть всегда
    CHECK(!result.has_more);

    bool found_expense = false, found_income = false, found_transfer = false;
    for (const auto& row : result.rows) {
        std::string uuid = row.at("operation_uuid").get<std::string>();
        if (uuid == op_expense) {
            CHECK_EQ(row.at("kind").get<std::string>(), std::string("expense"));
            CHECK_EQ(row.at("category_uuid").get<std::string>(), category_uuid);
            CHECK_EQ(row.at("wallet_uuid").get<std::string>(), wallet_a);
            CHECK_EQ(row.at("icon_sha256").get<std::string>(), icon.sha256_hex);
            found_expense = true;
        }
        if (uuid == op_income) {
            CHECK_EQ(row.at("kind").get<std::string>(), std::string("income"));
            CHECK_EQ(row.at("category_uuid").get<std::string>(), category_uuid);
            CHECK_EQ(row.at("wallet_uuid").get<std::string>(), wallet_a);
            found_income = true;
        }
        if (uuid == op_transfer) {
            CHECK_EQ(row.at("kind").get<std::string>(), std::string("transfer"));
            // NULL (не "no category/wallet у этого счёта", а буквально нет
            // ни категории, ни ОДНОГО кошелька — см. reports_repo.hpp).
            CHECK_EQ(row.at("category_uuid").get<std::string>(), std::string(""));
            CHECK_EQ(row.at("category_name").get<std::string>(), std::string(""));
            CHECK_EQ(row.at("icon_sha256").get<std::string>(), std::string(""));
            CHECK_EQ(row.at("wallet_uuid").get<std::string>(), std::string(""));
            CHECK_EQ(row.at("wallet_name").get<std::string>(), std::string(""));
            CHECK_EQ(row.at("from_uuid").get<std::string>(), wallet_a);
            CHECK_EQ(row.at("to_uuid").get<std::string>(), wallet_b);
            found_transfer = true;
        }
    }
    CHECK(found_expense);
    CHECK(found_income);
    CHECK(found_transfer);
}

// Пагинация — по прямому запросу (2026-09-12, "давай улучшим операции...
// по умолчанию 1000"): 5 операций, страницы по 2 — проверяет и сам курсор
// (страница 3 не теряет и не дублирует ни одной операции), и has_more
// ровно на границе (последняя страница НЕ полная — 1 из 2 — значит
// "подглядывающая" +1-строка уже не может прийти, has_more обязан быть
// false без всякого дополнительного похода в базу).
TEST(operations_for_period_pages_through_cursor_without_gaps_or_duplicates) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "PG" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_uuid = uuidgen::v4();

    std::vector<std::string> op_uuids;
    json ops = json::array();
    for (int i = 1; i <= 5; ++i) {
        std::string uuid = uuidgen::v4();
        op_uuids.push_back(uuid);
        ops.push_back(operation_row(uuid, "2026-07-0" + std::to_string(i) + "T10:00:00", category_uuid,
                                     wallet_uuid, std::to_string(i) + ".00"));
    }
    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", ops},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    // ORDER BY date DESC — страница 1: операции 5,4 (даты 07-05, 07-04).
    auto page1 = reports_repo::operations_for_period(*reports_db, identity.ledger_id, "2026-07-01T00:00:00",
                                                       "2026-07-31T23:59:59", *eq_currency, 2, "", "");
    REQUIRE(page1.rows.size() == static_cast<size_t>(2));
    CHECK(page1.has_more);
    CHECK(!page1.next_cursor_date.empty());
    CHECK_EQ(page1.rows[0].at("operation_uuid").get<std::string>(), op_uuids[4]); // 07-05
    CHECK_EQ(page1.rows[1].at("operation_uuid").get<std::string>(), op_uuids[3]); // 07-04

    // Страница 2 — операции 3,2 (07-03, 07-02).
    auto page2 = reports_repo::operations_for_period(*reports_db, identity.ledger_id, "2026-07-01T00:00:00",
                                                       "2026-07-31T23:59:59", *eq_currency, 2,
                                                       page1.next_cursor_date, page1.next_cursor_uuid);
    REQUIRE(page2.rows.size() == static_cast<size_t>(2));
    CHECK(page2.has_more);
    CHECK_EQ(page2.rows[0].at("operation_uuid").get<std::string>(), op_uuids[2]); // 07-03
    CHECK_EQ(page2.rows[1].at("operation_uuid").get<std::string>(), op_uuids[1]); // 07-02

    // Страница 3 — последняя, неполная (1 из 2) — has_more обязан быть
    // false, а не true "потому что запросили лимит 2".
    auto page3 = reports_repo::operations_for_period(*reports_db, identity.ledger_id, "2026-07-01T00:00:00",
                                                       "2026-07-31T23:59:59", *eq_currency, 2,
                                                       page2.next_cursor_date, page2.next_cursor_uuid);
    REQUIRE(page3.rows.size() == static_cast<size_t>(1));
    CHECK(!page3.has_more);
    CHECK(page3.next_cursor_date.empty());
    CHECK(page3.next_cursor_uuid.empty());
    CHECK_EQ(page3.rows[0].at("operation_uuid").get<std::string>(), op_uuids[0]); // 07-01
}

// incomplete — про СТРАНИЦУ, не про весь период: операция без курса лежит
// на первой странице (свежее по дате), вторая страница из операций С курсом
// обязана остаться incomplete == false.
TEST(operations_for_period_incomplete_is_scoped_to_the_page) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string cur_eq = uuidgen::v4();
    std::string cur_no_rate = uuidgen::v4();
    std::string iso_eq = "PE" + uuidgen::v4().substr(0, 1);
    std::string iso_no_rate = "PN" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_priced = uuidgen::v4();
    std::string wallet_unpriced = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(cur_eq, icon_uuid, iso_eq),
                                  currency_row(cur_no_rate, icon_uuid, iso_no_rate)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_priced, icon_uuid, "Priced " + wallet_priced),
                                  account_row(wallet_unpriced, icon_uuid, "Unpriced " + wallet_unpriced)})},
        {"account_operating", json::array({account_operating_row(wallet_priced, cur_eq),
                                            account_operating_row(wallet_unpriced, cur_no_rate)})},
        {"operation", json::array({
             // Свежее — без курса.
             operation_row(uuidgen::v4(), "2026-08-02T10:00:00", category_uuid, wallet_unpriced, "25.00"),
             // Старше — с курсом.
             operation_row(uuidgen::v4(), "2026-08-01T10:00:00", category_uuid, wallet_priced, "40.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso_eq, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto page1 = reports_repo::operations_for_period(*reports_db, identity.ledger_id, "2026-08-01T00:00:00",
                                                       "2026-08-31T23:59:59", *eq_currency, 1, "", "");
    REQUIRE(page1.rows.size() == static_cast<size_t>(1));
    CHECK(page1.has_more);
    CHECK(page1.incomplete); // единственная строка страницы — как раз unpriced

    auto page2 = reports_repo::operations_for_period(*reports_db, identity.ledger_id, "2026-08-01T00:00:00",
                                                       "2026-08-31T23:59:59", *eq_currency, 1,
                                                       page1.next_cursor_date, page1.next_cursor_uuid);
    REQUIRE(page2.rows.size() == static_cast<size_t>(1));
    CHECK(!page2.has_more);
    CHECK(!page2.incomplete); // priced — курс есть
}

// ---------------------------------------------------------------------------
// Фильтры ленты и итог (change add-operations-filters, wallet-api 000027).
// ---------------------------------------------------------------------------

namespace {

json operation_with_comment(const std::string& uuid, const std::string& date, const std::string& from_uuid,
                            const std::string& to_uuid, const std::string& amount, const std::string& comment) {
    json row = operation_row(uuid, date, from_uuid, to_uuid, amount);
    row["comment"] = comment;
    return row;
}

// Один леджер на все тесты фильтров: два кошелька в валюте эквивалента, один
// в валюте без курса, пять категорий. Суммы — целые, чтобы сравнение итогов
// не зависело от формата numeric.
struct FilterLedger {
    fixture::TestIdentity identity;
    std::string eq_currency;
    std::string w_tinkoff, w_cash, w_unpriced;
    std::string k_taxi, k_nanny, k_cafe, k_food, k_salary, k_misc;
    std::string e1, e2, e3, e4, e5, e6, e7, i1, t1, u1;
    static constexpr const char* kFrom = "2026-05-01T00:00:00";
    static constexpr const char* kTo = "2026-05-31T23:59:59";
};

FilterLedger seed_filter_ledger(db::Database& db, db::Database& reports_db) {
    FilterLedger l;
    l.identity = fixture::make_identity(db);
    std::string icon_uuid = fixture::make_reference_icon(db);
    std::string cur = uuidgen::v4();
    std::string cur_no_rate = uuidgen::v4();
    std::string iso = "FL" + uuidgen::v4().substr(0, 6);
    std::string iso_no_rate = "FN" + uuidgen::v4().substr(0, 6);
    for (auto* s : {&l.w_tinkoff, &l.w_cash, &l.w_unpriced, &l.k_taxi, &l.k_nanny, &l.k_cafe, &l.k_food,
                    &l.k_salary, &l.k_misc, &l.e1, &l.e2, &l.e3, &l.e4, &l.e5, &l.e6, &l.e7, &l.i1, &l.t1,
                    &l.u1}) {
        *s = uuidgen::v4();
    }

    json changes = {
        {"currency", json::array({currency_row(cur, icon_uuid, iso), currency_row(cur_no_rate, icon_uuid, iso_no_rate)})},
        {"account", json::array({
             account_row(l.w_tinkoff, icon_uuid, "Тинькофф"), account_row(l.w_cash, icon_uuid, "Наличные"),
             account_row(l.w_unpriced, icon_uuid, "Без курса"), account_row(l.k_taxi, icon_uuid, "Такси"),
             account_row(l.k_nanny, icon_uuid, "Няня"), account_row(l.k_cafe, icon_uuid, "Кафе"),
             account_row(l.k_food, icon_uuid, "Продукты"), account_row(l.k_salary, icon_uuid, "Зарплата"),
             account_row(l.k_misc, icon_uuid, "Разное"),
         })},
        {"account_operating", json::array({account_operating_row(l.w_tinkoff, cur),
                                            account_operating_row(l.w_cash, cur),
                                            account_operating_row(l.w_unpriced, cur_no_rate)})},
        {"operation", json::array({
             operation_with_comment(l.e1, "2026-05-01T10:00:00", l.w_tinkoff, l.k_taxi, "450.00", "Такси домой"),
             operation_with_comment(l.e2, "2026-05-02T10:00:00", l.w_cash, l.k_taxi, "300.00", "такси в аэропорт"),
             operation_with_comment(l.e3, "2026-05-03T10:00:00", l.w_tinkoff, l.k_cafe, "120.00", "Кофе 100% арабика"),
             operation_with_comment(l.e4, "2026-05-04T10:00:00", l.w_cash, l.k_cafe, "80.00", "кафе 100 рублей"),
             operation_with_comment(l.e5, "2026-05-05T10:00:00", l.w_tinkoff, l.k_nanny, "5000.00", "Няня за неделю"),
             operation_with_comment(l.i1, "2026-05-06T10:00:00", l.k_salary, l.w_tinkoff, "1000.00", "Зарплата"),
             operation_with_comment(l.t1, "2026-05-07T10:00:00", l.w_cash, l.w_tinkoff, "200.00", "перевод на карту"),
             operation_with_comment(l.e6, "2026-05-08T10:00:00", l.w_cash, l.k_food, "70.00", "новогодняя елка"),
             operation_with_comment(l.e7, "2026-05-09T10:00:00", l.w_tinkoff, l.k_food, "50.00", ""),
             operation_with_comment(l.u1, "2026-05-10T10:00:00", l.w_unpriced, l.k_misc, "10.00", "такси без курса"),
         })},
    };
    auto push = sync_repo::push_batch(db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq = reports_repo::resolve_eq_currency(reports_db, iso, l.identity.ledger_id);
    REQUIRE(eq.has_value());
    l.eq_currency = *eq;
    return l;
}

// Все страницы ленты с фильтром (limit маленький) — uuid по порядку обхода.
std::vector<json> all_filtered_rows(db::Database& reports_db, const FilterLedger& l,
                                    const reports_repo::OperationsFilter& filter, int limit) {
    std::vector<json> rows;
    std::string cursor_date, cursor_uuid;
    for (int guard = 0; guard < 10000; ++guard) {
        auto page = reports_repo::operations_for_period(reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                         FilterLedger::kTo, l.eq_currency, limit, cursor_date,
                                                         cursor_uuid, filter);
        for (const auto& r : page.rows) rows.push_back(r);
        if (!page.has_more) break;
        cursor_date = page.next_cursor_date;
        cursor_uuid = page.next_cursor_uuid;
    }
    return rows;
}

std::vector<std::string> sorted_uuids(const std::vector<json>& rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(r.at("operation_uuid").get<std::string>());
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::string> filtered_uuids(db::Database& reports_db, const FilterLedger& l,
                                        const reports_repo::OperationsFilter& filter) {
    return sorted_uuids(all_filtered_rows(reports_db, l, filter, 1000));
}

double as_number(const std::string& s) { return std::stod(s); }

} // namespace

// Каждый фильтр отдельно, списки по ИЛИ, фильтры по И.
TEST(operations_filter_each_filter_and_combinations) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_filter_ledger(*db, *reports_db);

    reports_repo::OperationsFilter f;
    f.category_uuids = {l.k_taxi};
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e1, l.e2}));

    f = {};
    f.category_uuids = {l.k_taxi, l.k_nanny}; // ИЛИ внутри списка
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e1, l.e2, l.e5}));

    f = {};
    f.wallet_uuids = {l.w_cash}; // перевод с «Наличных» — по кошельку-отправителю
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e2, l.e4, l.t1, l.e6}));

    f = {};
    f.kind = "income";
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.i1}));
    f.kind = "transfer";
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.t1}));
    f.kind = "expense";
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e1, l.e2, l.e3, l.e4, l.e5, l.e6, l.e7, l.u1}));

    f = {};
    f.category_uuids = {l.k_cafe};
    f.wallet_uuids = {l.w_tinkoff}; // И: «Кафе» и «Тинькофф»
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e3}));

    f = {};
    f.q = "такси";
    f.kind = "expense";
    f.wallet_uuids = {l.w_cash};
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e2}));
}

// Прямой перевод находится по любому из двух кошельков; uuid категории среди
// кошельков (и наоборот) ничего не находит; категория под фильтр перевода не тянет.
TEST(operations_filter_wallet_side_and_cross_kind_uuids) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_filter_ledger(*db, *reports_db);

    reports_repo::OperationsFilter f;
    f.wallet_uuids = {l.w_tinkoff}; // перевод «Наличные» → «Тинькофф» — по получателю
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e1, l.e3, l.e5, l.i1, l.t1, l.e7}));

    f = {};
    f.wallet_uuids = {l.k_taxi};
    auto r = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                  FilterLedger::kTo, l.eq_currency, 1000, "", "", f);
    CHECK(r.rows.empty());
    CHECK(r.has_total);
    CHECK_EQ(r.total_count, 0LL);

    f = {};
    f.category_uuids = {l.w_tinkoff, l.w_cash}; // uuid кошельков в фильтре категорий
    CHECK(filtered_uuids(*reports_db, l, f).empty());
}

// uuid категории другого леджера — 200 с пустым списком, чужие операции не видны.
TEST(operations_filter_foreign_ledger_uuid_finds_nothing) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto mine = seed_filter_ledger(*db, *reports_db);
    auto other = seed_filter_ledger(*db, *reports_db);

    reports_repo::OperationsFilter f;
    f.category_uuids = {other.k_taxi};
    auto r = reports_repo::operations_for_period(*reports_db, mine.identity.ledger_id, FilterLedger::kFrom,
                                                  FilterLedger::kTo, mine.eq_currency, 1000, "", "", f);
    CHECK(r.rows.empty());
    CHECK(r.has_total);
    CHECK_EQ(r.total_count, 0LL);
    CHECK_EQ(as_number(r.total_eq), 0.0);
    CHECK(!r.total_incomplete);
}

// q: без учёта регистра, ё = е, % и _ — буквально, пустой комментарий не совпадает.
TEST(operations_filter_q_case_yo_and_literal_wildcards) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_filter_ledger(*db, *reports_db);

    reports_repo::OperationsFilter f;
    f.q = "ЁЛКА"; // комментарий «новогодняя елка»
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e6}));

    f.q = "ТАКСИ";
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e1, l.e2, l.u1}));

    f.q = "100%"; // «кафе 100 рублей» не подходит
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e3}));

    f.q = "1_0"; // как LIKE совпало бы со «100», буквально — ни с чем
    CHECK(filtered_uuids(*reports_db, l, f).empty());

    f.q = "%";
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e3}));

    // Пустой комментарий (e7) не находится ни по какой подстроке.
    f.q = " ";
    auto rows = filtered_uuids(*reports_db, l, f);
    CHECK(std::find(rows.begin(), rows.end(), l.e7) == rows.end());
}

// Несколько вариантов q (search-text-variants): комментарий с ЛЮБЫМ из них,
// те же правила (регистр, ё = е, буквально); операция, подходящая под два
// варианта, — одна строка ленты и один раз в итоге.
TEST(operations_filter_q_variants_match_any_and_count_once) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_filter_ledger(*db, *reports_db);

    reports_repo::OperationsFilter f;
    f.qs = {"ЁЛКА", "кофе"}; // «новогодняя елка», «Кофе 100% арабика»
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e6, l.e3}));

    f.qs = {"1_0", "100%"}; // «1_0» буквально — ни с чем, «100%» — только e3
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e3}));

    // «такси в аэропорт» подходит под оба варианта — не задваивается.
    reports_repo::OperationsFilter one;
    one.q = "такси";
    one.kind = "expense";
    f = {};
    f.qs = {"такси", "аэропорт"};
    f.kind = "expense";
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e1, l.e2, l.u1}));
    auto a = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                  FilterLedger::kTo, l.eq_currency, 1000, "", "", one);
    auto b = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                  FilterLedger::kTo, l.eq_currency, 1000, "", "", f);
    CHECK(b.has_total);
    CHECK_EQ(b.total_count, 3LL);
    CHECK_EQ(b.total_eq, a.total_eq);
    CHECK_EQ(b.total_incomplete, a.total_incomplete);

    // Варианты по И с остальными фильтрами; пагинация по ним — без пропусков и повторов.
    f.wallet_uuids = {l.w_cash};
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e2}));
    f = {};
    f.qs = {"такси", "кафе", "кофе"};
    CHECK(sorted_uuids(all_filtered_rows(*reports_db, l, f, 2)) == sorted({l.e1, l.e2, l.u1, l.e3, l.e4}));
}

// Разбор повторяемого q: пустые отбрасываются, один вариант — в q, несколько —
// в qs, не больше 10, каждый не длиннее 100 символов.
TEST(operations_filter_parse_q_variants) {
    reports_repo::OperationsFilter f;
    using V = std::vector<std::string>;

    CHECK(!reports_repo::parse_operations_filter("", "", "", "", V{"", ""}, f).has_value());
    CHECK(!f.any());

    CHECK(!reports_repo::parse_operations_filter("", "", "", "", V{"", "такси"}, f).has_value());
    CHECK_EQ(f.q, std::string("такси"));
    CHECK(f.qs.empty());

    CHECK(!reports_repo::parse_operations_filter("", "", "", "", V{"стриж", "barber"}, f).has_value());
    CHECK(f.q.empty());
    CHECK((f.qs == V{"стриж", "barber"}));
    CHECK(f.any());

    V ten(10, "x"), eleven(11, "x");
    CHECK(!reports_repo::parse_operations_filter("", "", "", "", ten, f).has_value());
    CHECK_EQ(f.qs.size(), static_cast<size_t>(10));
    CHECK_EQ(reports_repo::parse_operations_filter("", "", "", "", eleven, f).value_or(""),
             std::string("q: не больше 10 значений"));

    std::string cyr101;
    for (int i = 0; i < 101; ++i) cyr101 += "ж";
    CHECK_EQ(reports_repo::parse_operations_filter("", "", "", "", V{"такси", cyr101}, f).value_or(""),
             std::string("q: не длиннее 100 символов"));
}

// Пагинация по отфильтрованному набору: 3000 операций, 5 подходящих, limit 2 —
// страницы 2, 2, 1, у последней has_more: false, каждая ровно один раз.
// Две подходящие — с одной датой (порядок по (date, uuid)).
TEST(operations_filter_pages_without_gaps_or_duplicates) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();

    FilterLedger l;
    l.identity = fixture::make_identity(*db);
    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string cur = uuidgen::v4();
    std::string iso = "FP" + uuidgen::v4().substr(0, 6);
    std::string wallet = uuidgen::v4();
    std::string category = uuidgen::v4();

    std::vector<std::string> expected;
    json ops = json::array();
    for (int i = 0; i < 3000; ++i) {
        int minutes = (i == 501 ? 500 : i) * 10; // 500 и 501 — одна и та же дата
        char date[32];
        std::snprintf(date, sizeof(date), "2026-05-%02dT%02d:%02d:00", 1 + minutes / 1440, (minutes % 1440) / 60,
                      minutes % 60);
        bool match = i == 7 || i == 500 || i == 501 || i == 1700 || i == 2999;
        std::string uuid = uuidgen::v4();
        if (match) expected.push_back(uuid);
        ops.push_back(operation_with_comment(uuid, date, wallet, category, "1.00",
                                             match ? "Редкое совпадение" : "обычная трата"));
    }
    json changes = {
        {"currency", json::array({currency_row(cur, icon_uuid, iso)})},
        {"account", json::array({account_row(wallet, icon_uuid, "Кошелёк"), account_row(category, icon_uuid, "Категория")})},
        {"account_operating", json::array({account_operating_row(wallet, cur)})},
        {"operation", ops},
    };
    auto push = sync_repo::push_batch(*db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);
    auto eq = reports_repo::resolve_eq_currency(*reports_db, iso, l.identity.ledger_id);
    REQUIRE(eq.has_value());
    l.eq_currency = *eq;

    reports_repo::OperationsFilter f;
    f.q = "редкое";
    f.kind = "expense";

    std::vector<std::size_t> page_sizes;
    std::vector<json> rows;
    std::string cursor_date, cursor_uuid;
    bool last_has_more = true;
    for (int guard = 0; guard < 10 && last_has_more; ++guard) {
        auto page = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                         FilterLedger::kTo, l.eq_currency, 2, cursor_date,
                                                         cursor_uuid, f);
        page_sizes.push_back(page.rows.size());
        for (const auto& r : page.rows) rows.push_back(r);
        CHECK_EQ(page.total_count, 5LL); // итог — по всем страницам, на каждой
        last_has_more = page.has_more;
        cursor_date = page.next_cursor_date;
        cursor_uuid = page.next_cursor_uuid;
    }
    CHECK(page_sizes == (std::vector<std::size_t>{2, 2, 1}));
    CHECK(!last_has_more);
    CHECK(sorted_uuids(rows) == sorted(expected));
}

// Итог по фильтру: за весь период при limit меньше числа совпадений; с kind —
// неотрицательная сумма; без kind — сальдо, прямой перевод даёт 0, но входит в
// total_count; операция без курса — incomplete и не в сумме; нет совпадений — 0/false/0.
TEST(operations_total_sign_rule_incomplete_and_whole_period) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_filter_ledger(*db, *reports_db);

    auto page = [&](const reports_repo::OperationsFilter& f, int limit) {
        return reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                    FilterLedger::kTo, l.eq_currency, limit, "", "", f);
    };

    // Такси, только расходы: 450 + 300, страница из одной строки.
    reports_repo::OperationsFilter f;
    f.category_uuids = {l.k_taxi};
    f.kind = "expense";
    auto r = page(f, 1);
    CHECK_EQ(r.rows.size(), static_cast<size_t>(1));
    CHECK(r.has_more);
    CHECK(r.has_total);
    CHECK_EQ(as_number(r.total_eq), 750.0);
    CHECK_EQ(r.total_count, 2LL);
    CHECK(!r.total_incomplete);

    // Сальдо «Тинькофф»: доход 1000 − расходы (450 + 120 + 5000 + 50), перевод 200 — 0, но в счёте.
    f = {};
    f.wallet_uuids = {l.w_tinkoff};
    r = page(f, 2);
    CHECK_EQ(as_number(r.total_eq), -4620.0);
    CHECK_EQ(r.total_count, 6LL);

    // Сальдо в минус: только расходы на няню.
    f = {};
    f.q = "няня";
    r = page(f, 1000);
    CHECK_EQ(as_number(r.total_eq), -5000.0);
    CHECK_EQ(r.total_count, 1LL);

    // kind=transfer — неотрицательная сумма переводов.
    f = {};
    f.kind = "transfer";
    r = page(f, 1000);
    CHECK_EQ(as_number(r.total_eq), 200.0);
    CHECK_EQ(r.total_count, 1LL);

    // Операция без курса: в счёте, не в сумме, incomplete на всех страницах.
    f = {};
    f.category_uuids = {l.k_taxi, l.k_misc};
    r = page(f, 1); // первая страница — u1 (самая свежая), без курса
    CHECK_EQ(as_number(r.total_eq), -750.0);
    CHECK_EQ(r.total_count, 3LL);
    CHECK(r.total_incomplete);
    auto r2 = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                   FilterLedger::kTo, l.eq_currency, 1, r.next_cursor_date,
                                                   r.next_cursor_uuid, f);
    CHECK(!r2.incomplete); // страница с курсом
    CHECK(r2.total_incomplete);  // итог — по всему периоду

    // Ничего не подошло.
    f = {};
    f.q = "нет такого комментария";
    r = page(f, 1000);
    CHECK(r.rows.empty());
    CHECK(r.has_total);
    CHECK_EQ(as_number(r.total_eq), 0.0);
    CHECK(!r.total_incomplete);
    CHECK_EQ(r.total_count, 0LL);

    // Без фильтров итога нет.
    r = page(reports_repo::OperationsFilter{}, 1000);
    CHECK(!r.has_total);
    CHECK_EQ(r.rows.size(), static_cast<size_t>(10));
}

// Согласованность двух SQL-функций: matched = число строк по всем страницам,
// total_eq = их сумма по правилу знака, incomplete = есть строка без amount_eq.
TEST(operations_total_matches_all_pages) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_filter_ledger(*db, *reports_db);

    std::vector<reports_repo::OperationsFilter> filters(7);
    filters[0].wallet_uuids = {l.w_cash};
    filters[1].wallet_uuids = {l.w_tinkoff, l.w_unpriced};
    filters[2].kind = "expense";
    filters[3].q = "такси";
    filters[4].category_uuids = {l.k_cafe, l.k_food, l.k_misc};
    filters[5].category_uuids = {l.k_salary};
    filters[5].wallet_uuids = {l.w_tinkoff};
    filters[6].kind = "income";
    filters[6].q = "а";

    for (const auto& f : filters) {
        auto rows = all_filtered_rows(*reports_db, l, f, 2);
        double sum = 0.0;
        bool incomplete = false;
        for (const auto& row : rows) {
            std::string amount_eq = row.at("amount_eq").get<std::string>();
            if (amount_eq.empty()) {
                incomplete = true;
                continue;
            }
            double v = as_number(amount_eq);
            std::string kind = row.at("kind").get<std::string>();
            if (!f.kind.empty()) sum += v;
            else if (kind == "income") sum += v;
            else if (kind == "expense") sum -= v;
        }
        auto total = reports_repo::operations_total(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                    FilterLedger::kTo, l.eq_currency, f);
        CHECK_EQ(total.matched, static_cast<long long>(rows.size()));
        CHECK(std::abs(as_number(total.total_eq) - sum) < 1e-9);
        CHECK_EQ(total.incomplete, incomplete);
    }
}

// Проверка ввода (spec «Проверка параметров фильтра») — чистая функция, без базы:
// тексты 400 дословно. Через HTTP её зовёт handle_operations.
TEST(operations_filter_parse_rejects_bad_input_with_exact_texts) {
    reports_repo::OperationsFilter f;
    std::string u = uuidgen::v4();

    CHECK(!reports_repo::parse_operations_filter("", "", "", "", f).has_value());
    CHECK(!f.any()); // пустые значения — фильтров нет

    CHECK_EQ(reports_repo::parse_operations_filter("", "abc", "", "", f).value_or(""),
             std::string("wallet_uuid: некорректный uuid"));
    CHECK_EQ(reports_repo::parse_operations_filter(u + ",", "", "", "", f).value_or(""),
             std::string("category_uuid: некорректный uuid"));
    CHECK_EQ(reports_repo::parse_operations_filter("," + u, "", "", "", f).value_or(""),
             std::string("category_uuid: некорректный uuid"));

    std::string fifty = u, fifty_one;
    for (int i = 1; i < 50; ++i) fifty += "," + u; // дубли допустимы
    fifty_one = fifty + "," + u;
    CHECK(!reports_repo::parse_operations_filter(fifty, "", "", "", f).has_value());
    CHECK_EQ(f.category_uuids.size(), static_cast<size_t>(50));
    CHECK_EQ(reports_repo::parse_operations_filter("", fifty_one, "", "", f).value_or(""),
             std::string("wallet_uuid: не больше 50 значений"));
    CHECK_EQ(reports_repo::parse_operations_filter(fifty_one, "", "", "", f).value_or(""),
             std::string("category_uuid: не больше 50 значений"));

    CHECK_EQ(reports_repo::parse_operations_filter("", "", "Expense", "", f).value_or(""),
             std::string("kind должен быть expense, income, transfer, revaluation или debt"));
    for (const char* k : {"expense", "income", "transfer", "revaluation", "debt"}) {
        CHECK(!reports_repo::parse_operations_filter("", "", k, "", f).has_value());
    }

    std::string cyr100;
    for (int i = 0; i < 100; ++i) cyr100 += "ж"; // 200 байт UTF-8
    CHECK(!reports_repo::parse_operations_filter("", "", "", cyr100, f).has_value());
    CHECK_EQ(f.q, cyr100);
    CHECK_EQ(reports_repo::parse_operations_filter("", "", "", cyr100 + "ж", f).value_or(""),
             std::string("q: не длиннее 100 символов"));
    CHECK_EQ(reports_repo::parse_operations_filter("", "", "", std::string("\xD0", 1), f).value_or(""),
             std::string("q: не длиннее 100 символов")); // битый UTF-8 — тот же текст

    CHECK(!reports_repo::parse_operations_filter(u, u, "income", "такси", f).has_value());
    CHECK(f.any());
    CHECK_EQ(f.category_uuids.size(), static_cast<size_t>(1));
    CHECK_EQ(f.wallet_uuids.size(), static_cast<size_t>(1));
}

// "Кошельки и остатки" — три кошелька: один тронут только КАТЕГОРИЙНОЙ
// операцией (расход), один тронут только ПЕРЕВОДОМ кошелёк -> кошелёк (сюда
// f_income_for_period/f_expanse_for_period вообще не заглянули бы — прямая
// проверка того, что turnover считает это движение тоже), и один БЕЗ единой
// операции за период вовсе — обязан всё равно попасть в ответ с "0.00"/"0.00"
// (zero-fill, см. reports_repo.hpp).
TEST(wallet_turnover_for_period_counts_transfers_and_zero_fills_untouched_wallet) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string icon_uuid = icon.uuid;
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "WT" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_expense = uuidgen::v4();  // тронут категорийным расходом
    std::string wallet_transfer_from = uuidgen::v4();
    std::string wallet_transfer_to = uuidgen::v4();
    std::string wallet_untouched = uuidgen::v4(); // без единой операции в периоде

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_expense, icon_uuid, "Expense wallet " + wallet_expense),
                                  account_row(wallet_transfer_from, icon_uuid, "From " + wallet_transfer_from),
                                  account_row(wallet_transfer_to, icon_uuid, "To " + wallet_transfer_to),
                                  account_row(wallet_untouched, icon_uuid, "Untouched " + wallet_untouched)})},
        {"account_operating", json::array({account_operating_row(wallet_expense, currency_uuid),
                                            account_operating_row(wallet_transfer_from, currency_uuid),
                                            account_operating_row(wallet_transfer_to, currency_uuid),
                                            account_operating_row(wallet_untouched, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-07-05T10:00:00", wallet_expense, category_uuid, "40.00"),
             operation_row(uuidgen::v4(), "2026-07-06T10:00:00", wallet_transfer_from, wallet_transfer_to,
                           "15.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    // eq_currency — та же валюта, что и у всех кошельков (курс на саму себя
    // всегда 1, см. v_currency_conversion_rate.sql) — turnover_*_eq обязан
    // СОВПАСТЬ со "своей" суммой дословно, самый простой способ проверить,
    // что eq вообще посчитан, а не просто скопирован/забыт.
    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::wallet_turnover_for_period(
        *reports_db, identity.ledger_id, "2026-07-01T00:00:00", "2026-07-31T23:59:59", *eq_currency);
    CHECK(!result.incomplete); // курс на саму себя есть всегда

    bool found_expense = false, found_from = false, found_to = false, found_untouched = false;
    for (const auto& row : result.rows) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        CHECK_EQ(row.at("icon_sha256").get<std::string>(), icon.sha256_hex);
        CHECK(!row.at("is_archived").get<bool>()); // все заведены с is_archived=false по умолчанию
        if (uuid == wallet_expense) {
            CHECK_EQ(row.at("turnover_in").get<std::string>(), std::string("0.00"));
            CHECK_EQ(row.at("turnover_out").get<std::string>(), std::string("40.00"));
            CHECK_EQ(row.at("turnover_in_eq").get<std::string>(), std::string("0.00"));
            CHECK_EQ(row.at("turnover_out_eq").get<std::string>(), std::string("40.00"));
            found_expense = true;
        }
        if (uuid == wallet_transfer_from) {
            // Перевод кошелёк -> кошелёк: f_income_for_period/f_expanse_for_period
            // его вообще не видят (обе стороны operating), turnover обязан.
            CHECK_EQ(row.at("turnover_in").get<std::string>(), std::string("0.00"));
            CHECK_EQ(row.at("turnover_out").get<std::string>(), std::string("15.00"));
            CHECK_EQ(row.at("turnover_out_eq").get<std::string>(), std::string("15.00"));
            found_from = true;
        }
        if (uuid == wallet_transfer_to) {
            CHECK_EQ(row.at("turnover_in").get<std::string>(), std::string("15.00"));
            CHECK_EQ(row.at("turnover_out").get<std::string>(), std::string("0.00"));
            CHECK_EQ(row.at("turnover_in_eq").get<std::string>(), std::string("15.00"));
            found_to = true;
        }
        if (uuid == wallet_untouched) {
            CHECK_EQ(row.at("turnover_in").get<std::string>(), std::string("0.00"));
            CHECK_EQ(row.at("turnover_out").get<std::string>(), std::string("0.00"));
            CHECK_EQ(row.at("turnover_in_eq").get<std::string>(), std::string("0.00"));
            CHECK_EQ(row.at("turnover_out_eq").get<std::string>(), std::string("0.00"));
            found_untouched = true;
        }
    }
    CHECK(found_expense);
    CHECK(found_from);
    CHECK(found_to);
    CHECK(found_untouched);

    // Категория (не кошелёк — нет account_operating) не должна попасть в
    // список вовсе: v_account_operating её не видит.
    for (const auto& row : result.rows) {
        CHECK(row.at("account_uuid").get<std::string>() != category_uuid);
    }
}

// Операция ровно на границе периода (p_from) и ровно на другой границе
// (p_to) — обе включительны, в отличие от f_balance_to_date (см. её
// комментарий): регрессия здесь означала бы, что кто-то случайно поменял
// BETWEEN на строгое неравенство.
TEST(wallet_turnover_for_period_includes_both_boundary_dates) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "WB" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             // Приход РОВНО на p_from.
             operation_row(uuidgen::v4(), "2026-08-01T00:00:00", category_uuid, wallet_uuid, "10.00"),
             // Расход РОВНО на p_to.
             operation_row(uuidgen::v4(), "2026-08-31T23:59:59", wallet_uuid, category_uuid, "7.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::wallet_turnover_for_period(
        *reports_db, identity.ledger_id, "2026-08-01T00:00:00", "2026-08-31T23:59:59", *eq_currency);

    bool found = false;
    for (const auto& row : result.rows) {
        if (row.at("account_uuid").get<std::string>() != wallet_uuid) continue;
        CHECK_EQ(row.at("turnover_in").get<std::string>(), std::string("10.00"));
        CHECK_EQ(row.at("turnover_out").get<std::string>(), std::string("7.00"));
        CHECK_EQ(row.at("turnover_in_eq").get<std::string>(), std::string("10.00"));
        CHECK_EQ(row.at("turnover_out_eq").get<std::string>(), std::string("7.00"));
        found = true;
    }
    CHECK(found);
}

// Батч-баланс на несколько дат — по прямому запросу ("сервер научится
// отдавать балком за период для нескольких кошельков"). Три даты: ДО первой
// операции (леджер молчал — обязан вернуться пустым блоком, а не пропасть
// из ответа вовсе), между операциями и после обеих — проверяет разом, что
// результата РОВНО три блока, в исходном порядке, и что "дата без кошелька"
// не сдвигает индексацию последующих дат.
TEST(balances_at_dates_returns_one_block_per_date_in_order_including_empty_ones) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "BD" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-09-05T10:00:00", category_uuid, wallet_uuid, "100.00"),
             operation_row(uuidgen::v4(), "2026-09-15T10:00:00", category_uuid, wallet_uuid, "50.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    std::vector<std::string> dates = {
        "2026-09-01T00:00:00", // до первой операции — леджер молчал, ни одного кошелька с движением
        "2026-09-10T00:00:00", // после первой (+100), до второй
        "2026-09-20T00:00:00", // после обеих (+150)
    };
    auto result = reports_repo::balances_at_dates(*reports_db, identity.ledger_id, dates, *eq_currency);

    REQUIRE(result.dates.size() == static_cast<size_t>(3));
    // date — эхо запроса дословно, не то, что вернул/нормализовал Postgres
    // (см. reports_repo.hpp) — иначе "2026-09-01T00:00:00" превратилось бы в
    // "2026-09-01 00:00:00" и сравнение по индексу стало бы хрупким на
    // стороне клиента.
    CHECK_EQ(result.dates[0].at("date").get<std::string>(), dates[0]);
    CHECK_EQ(result.dates[1].at("date").get<std::string>(), dates[1]);
    CHECK_EQ(result.dates[2].at("date").get<std::string>(), dates[2]);

    CHECK(result.dates[0].at("accounts").empty());
    CHECK_EQ(result.dates[0].at("total_eq").get<std::string>(), std::string("0"));
    CHECK(!result.dates[0].at("incomplete").get<bool>());

    REQUIRE(result.dates[1].at("accounts").size() == static_cast<size_t>(1));
    CHECK_EQ(result.dates[1].at("accounts")[0].at("account_uuid").get<std::string>(), wallet_uuid);
    CHECK_EQ(result.dates[1].at("accounts")[0].at("balance").get<std::string>(), std::string("100.00"));
    CHECK_EQ(result.dates[1].at("accounts")[0].at("balance_eq").get<std::string>(), std::string("100.00"));

    REQUIRE(result.dates[2].at("accounts").size() == static_cast<size_t>(1));
    CHECK_EQ(result.dates[2].at("accounts")[0].at("balance").get<std::string>(), std::string("150.00"));
    CHECK(!result.dates[2].at("incomplete").get<bool>());
}

// Операция РОВНО на запрошенную дату — f_balance_to_dates считает СТРОГО ДО
// (тот же принцип, что f_balance_to_date, см. её комментарий), значит эта
// операция ещё не должна попасть в баланс. Регрессия здесь означала бы, что
// кто-то поменял "<" на "<=" при портировании на много дат разом.
TEST(balances_at_dates_excludes_operation_exactly_on_the_requested_date) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "BX" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-10-10T00:00:00", category_uuid, wallet_uuid, "30.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    std::vector<std::string> dates = {
        "2026-10-10T00:00:00", // РОВНО момент операции — граница не включается, кошелёк ещё "не тронут"
        "2026-10-10T00:00:01", // на секунду позже — операция уже должна попасть
    };
    auto result = reports_repo::balances_at_dates(*reports_db, identity.ledger_id, dates, *eq_currency);

    REQUIRE(result.dates.size() == static_cast<size_t>(2));
    CHECK(result.dates[0].at("accounts").empty());
    REQUIRE(result.dates[1].at("accounts").size() == static_cast<size_t>(1));
    CHECK_EQ(result.dates[1].at("accounts")[0].at("balance").get<std::string>(), std::string("30.00"));
}

// Одна и та же дата дважды во входном массиве — не запрещено (см.
// reports_repo.hpp у balances_at_dates: дубликаты просто не убираются,
// вызывающий код на клиенте намеренно их не дедуплицирует, см.
// wallet-web/site/app.js::refreshWalletBalances). Оба блока обязаны
// получиться ОДИНАКОВЫМИ и полными, ни один индекс не должен "потеряться"
// из-за совпадения дат.
TEST(balances_at_dates_handles_duplicate_dates_in_request) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "BY" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-11-05T10:00:00", category_uuid, wallet_uuid, "20.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    std::string same_date = "2026-11-10T00:00:00";
    std::vector<std::string> dates = {same_date, same_date};
    auto result = reports_repo::balances_at_dates(*reports_db, identity.ledger_id, dates, *eq_currency);

    REQUIRE(result.dates.size() == static_cast<size_t>(2));
    for (const auto& block : result.dates) {
        CHECK_EQ(block.at("date").get<std::string>(), same_date);
        REQUIRE(block.at("accounts").size() == static_cast<size_t>(1));
        CHECK_EQ(block.at("accounts")[0].at("balance").get<std::string>(), std::string("20.00"));
        CHECK_EQ(block.at("total_eq").get<std::string>(), std::string("20.00"));
    }
}

// Курса до eq_currency нет вовсе (нет ни одной currency_pair/currency_pair_rate
// для этой валюты) — та же проверка "sum() пропускает NULL и надо смотреть
// incomplete отдельно", что и во всех остальных отчётах (см. f_balance_to_date.sql,
// "Отсутствие COALESCE на курсе"). Два кошелька: один в валюте эквивалента
// (курс на саму себя есть всегда), один в валюте без единой пары — их
// balance_eq обязаны отличаться (число vs пустая строка), а признак
// incomplete блока — стать true из-за ВТОРОГО, не первого.
TEST(balances_at_dates_marks_incomplete_when_rate_missing_for_one_wallet) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string cur_eq = uuidgen::v4();   // валюта эквивалента — курс на саму себя есть всегда
    std::string cur_no_rate = uuidgen::v4(); // без единой currency_pair — курса к cur_eq нет вовсе
    std::string iso_eq = "NE" + uuidgen::v4().substr(0, 1);
    std::string iso_no_rate = "NR" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_priced = uuidgen::v4();
    std::string wallet_unpriced = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(cur_eq, icon_uuid, iso_eq),
                                  currency_row(cur_no_rate, icon_uuid, iso_no_rate)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_priced, icon_uuid, "Priced " + wallet_priced),
                                  account_row(wallet_unpriced, icon_uuid, "Unpriced " + wallet_unpriced)})},
        {"account_operating", json::array({account_operating_row(wallet_priced, cur_eq),
                                            account_operating_row(wallet_unpriced, cur_no_rate)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-12-01T10:00:00", category_uuid, wallet_priced, "40.00"),
             operation_row(uuidgen::v4(), "2026-12-01T10:00:00", category_uuid, wallet_unpriced, "25.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso_eq, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    std::vector<std::string> dates = {"2026-12-31T23:59:59"};
    auto result = reports_repo::balances_at_dates(*reports_db, identity.ledger_id, dates, *eq_currency);

    REQUIRE(result.dates.size() == static_cast<size_t>(1));
    const auto& block = result.dates[0];
    REQUIRE(block.at("accounts").size() == static_cast<size_t>(2));
    CHECK(block.at("incomplete").get<bool>()); // из-за wallet_unpriced — курса к нему нет

    bool found_priced = false, found_unpriced = false;
    for (const auto& row : block.at("accounts")) {
        std::string uuid = row.at("account_uuid").get<std::string>();
        if (uuid == wallet_priced) {
            CHECK_EQ(row.at("balance").get<std::string>(), std::string("40.00"));
            CHECK_EQ(row.at("balance_eq").get<std::string>(), std::string("40.00")); // курс на саму себя — 1
            found_priced = true;
        }
        if (uuid == wallet_unpriced) {
            CHECK_EQ(row.at("balance").get<std::string>(), std::string("25.00")); // своя валюта посчитана
            CHECK_EQ(row.at("balance_eq").get<std::string>(), std::string("")); // а эквивалент — не нашёлся ("" = SQL NULL, см. db.hpp)
            found_unpriced = true;
        }
    }
    CHECK(found_priced);
    CHECK(found_unpriced);
    // total_eq — сумма БЕЗ unpriced (sum() молча пропускает NULL, см. f_balance_to_date.sql) — ровно 40.00, не 65.00.
    CHECK_EQ(block.at("total_eq").get<std::string>(), std::string("40.00"));
}

// Несколько кошельков на ОДНУ дату — сортировка ВНУТРИ блока по эквиваленту
// убыв. (ORDER BY d.idx, eq_value DESC NULLS LAST в .sql), не по алфавиту и
// не по порядку вставки: тот же принцип, что и у f_balance_to_date.
TEST(balances_at_dates_orders_accounts_within_one_date_by_eq_desc) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "OR" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_small = uuidgen::v4();
    std::string wallet_big = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_small, icon_uuid, "Small " + wallet_small),
                                  account_row(wallet_big, icon_uuid, "Big " + wallet_big)})},
        {"account_operating", json::array({account_operating_row(wallet_small, currency_uuid),
                                            account_operating_row(wallet_big, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2027-01-05T10:00:00", category_uuid, wallet_small, "10.00"),
             operation_row(uuidgen::v4(), "2027-01-05T10:00:00", category_uuid, wallet_big, "500.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    std::vector<std::string> dates = {"2027-01-31T23:59:59"};
    auto result = reports_repo::balances_at_dates(*reports_db, identity.ledger_id, dates, *eq_currency);

    REQUIRE(result.dates.size() == static_cast<size_t>(1));
    const auto& accounts = result.dates[0].at("accounts");
    REQUIRE(accounts.size() == static_cast<size_t>(2));
    CHECK_EQ(accounts[0].at("account_uuid").get<std::string>(), wallet_big);
    CHECK_EQ(accounts[1].at("account_uuid").get<std::string>(), wallet_small);
}

// f_balance_series раньше не имела ни одного теста в этом файле (переносили
// из wallet-api как есть) — по прямому запросу, 2026-09-12 ("баланс за раз,
// инкрементально"): её тело переписано на кумулятивный проход (000018), и
// именно этот набор тестов — единственная защита от регрессии при такой
// правке. Мирроит покрытие balances_at_dates выше (та же формула баланса),
// шаг 'day' — самый простой для рассуждений о границах бакета.

TEST(balance_series_zero_fills_buckets_including_one_before_first_operation) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "SD" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-09-02T10:00:00", category_uuid, wallet_uuid, "100.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    // 3 дневных бакета: 09-01 (до операции — леджер молчал), 09-02 (операция
    // случилась ВНУТРИ него), 09-03 (после, без новых операций).
    auto result = reports_repo::balance_series(*reports_db, identity.ledger_id, "2026-09-01", "2026-09-03",
                                                "day", *eq_currency, true);

    REQUIRE(result.buckets.size() == static_cast<size_t>(3));
    CHECK(result.buckets[0].at("accounts").empty());
    CHECK_EQ(result.buckets[0].at("total_eq").get<std::string>(), std::string("0"));
    CHECK(!result.buckets[0].at("incomplete").get<bool>());

    REQUIRE(result.buckets[1].at("accounts").size() == static_cast<size_t>(1));
    CHECK_EQ(result.buckets[1].at("accounts")[0].at("balance").get<std::string>(), std::string("100.00"));

    REQUIRE(result.buckets[2].at("accounts").size() == static_cast<size_t>(1));
    CHECK_EQ(result.buckets[2].at("accounts")[0].at("balance").get<std::string>(), std::string("100.00"));
}

// Операция РОВНО в момент границы бакета ("день после конца", см.
// f_balance_series.sql) — должна попасть в СЛЕДУЮЩИЙ бакет, не в тот, чью
// границу она задевает: тот же принцип "строго до", что и у
// balances_at_dates_excludes_operation_exactly_on_the_requested_date выше,
// только здесь граница — не входной параметр, а bucket_end+1, вычисленный
// самой функцией. Регрессия здесь означала бы, что тай-брейк
// is_checkpoint DESC (см. комментарий f_balance_series.sql) перепутан.
TEST(balance_series_excludes_operation_exactly_on_bucket_boundary) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "SX" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_uuid = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_uuid, icon_uuid, "Wallet " + wallet_uuid)})},
        {"account_operating", json::array({account_operating_row(wallet_uuid, currency_uuid)})},
        {"operation", json::array({
             // Ровно полночь 10-11 — граница между бакетом 10-10 и 10-11.
             operation_row(uuidgen::v4(), "2026-10-11T00:00:00", category_uuid, wallet_uuid, "30.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::balance_series(*reports_db, identity.ledger_id, "2026-10-10", "2026-10-11",
                                                "day", *eq_currency, true);

    REQUIRE(result.buckets.size() == static_cast<size_t>(2));
    CHECK(result.buckets[0].at("accounts").empty()); // 10-10: граница не включается, счёт ещё "не тронут"
    REQUIRE(result.buckets[1].at("accounts").size() == static_cast<size_t>(1));
    CHECK_EQ(result.buckets[1].at("accounts")[0].at("balance").get<std::string>(), std::string("30.00"));
}

// incomplete — bool_or ТОЛЬКО по бакетам с реальными счетами (см. её
// комментарий в reports_repo.cpp): пустой бакет ДО первой операции не должен
// стать "неполным" только из-за отсутствия account_uuid.
TEST(balance_series_marks_incomplete_only_for_buckets_with_accounts) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string cur_eq = uuidgen::v4();
    std::string cur_no_rate = uuidgen::v4();
    std::string iso_eq = "SE" + uuidgen::v4().substr(0, 1);
    std::string iso_no_rate = "SN" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_priced = uuidgen::v4();
    std::string wallet_unpriced = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(cur_eq, icon_uuid, iso_eq),
                                  currency_row(cur_no_rate, icon_uuid, iso_no_rate)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_priced, icon_uuid, "Priced " + wallet_priced),
                                  account_row(wallet_unpriced, icon_uuid, "Unpriced " + wallet_unpriced)})},
        {"account_operating", json::array({account_operating_row(wallet_priced, cur_eq),
                                            account_operating_row(wallet_unpriced, cur_no_rate)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-12-02T10:00:00", category_uuid, wallet_priced, "40.00"),
             operation_row(uuidgen::v4(), "2026-12-02T10:00:00", category_uuid, wallet_unpriced, "25.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso_eq, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::balance_series(*reports_db, identity.ledger_id, "2026-12-01", "2026-12-02",
                                                "day", *eq_currency, true);

    REQUIRE(result.buckets.size() == static_cast<size_t>(2));
    CHECK(result.buckets[0].at("accounts").empty());
    CHECK(!result.buckets[0].at("incomplete").get<bool>()); // пустой бакет — не "неполный"

    REQUIRE(result.buckets[1].at("accounts").size() == static_cast<size_t>(2));
    CHECK(result.buckets[1].at("incomplete").get<bool>()); // wallet_unpriced — курса к нему нет
    CHECK_EQ(result.buckets[1].at("total_eq").get<std::string>(), std::string("40.00")); // без unpriced
}

// Несколько кошельков в одном бакете — сортировка по эквиваленту убыв., тот
// же принцип, что и у balances_at_dates_orders_accounts_within_one_date_by_eq_desc.
TEST(balance_series_orders_accounts_within_one_bucket_by_eq_desc) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "SO" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_small = uuidgen::v4();
    std::string wallet_big = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon_uuid, iso)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_small, icon_uuid, "Small " + wallet_small),
                                  account_row(wallet_big, icon_uuid, "Big " + wallet_big)})},
        {"account_operating", json::array({account_operating_row(wallet_small, currency_uuid),
                                            account_operating_row(wallet_big, currency_uuid)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2027-02-05T10:00:00", category_uuid, wallet_small, "10.00"),
             operation_row(uuidgen::v4(), "2027-02-05T10:00:00", category_uuid, wallet_big, "500.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto result = reports_repo::balance_series(*reports_db, identity.ledger_id, "2027-02-05", "2027-02-05",
                                                "day", *eq_currency, true);

    REQUIRE(result.buckets.size() == static_cast<size_t>(1));
    const auto& accounts = result.buckets[0].at("accounts");
    REQUIRE(accounts.size() == static_cast<size_t>(2));
    CHECK_EQ(accounts[0].at("account_uuid").get<std::string>(), wallet_big);
    CHECK_EQ(accounts[1].at("account_uuid").get<std::string>(), wallet_small);
}

// Режим по умолчанию у ручки (with_accounts=false) — итог агрегирует сама
// база (GROUP BY бакета), поэтому отдельно сверяем, что total_eq/incomplete
// совпадают с веткой по счетам, а поля accounts в блоке нет вовсе.
TEST(balance_series_totals_only_matches_per_account_totals) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string cur_eq = uuidgen::v4();
    std::string cur_no_rate = uuidgen::v4();
    std::string iso_eq = "ST" + uuidgen::v4().substr(0, 1);
    std::string iso_no_rate = "SU" + uuidgen::v4().substr(0, 1);
    std::string category_uuid = uuidgen::v4();
    std::string wallet_a = uuidgen::v4();
    std::string wallet_b = uuidgen::v4();
    std::string wallet_unpriced = uuidgen::v4();

    json changes = {
        {"currency", json::array({currency_row(cur_eq, icon_uuid, iso_eq),
                                  currency_row(cur_no_rate, icon_uuid, iso_no_rate)})},
        {"account", json::array({account_row(category_uuid, icon_uuid, "Cat " + category_uuid),
                                  account_row(wallet_a, icon_uuid, "A " + wallet_a),
                                  account_row(wallet_b, icon_uuid, "B " + wallet_b),
                                  account_row(wallet_unpriced, icon_uuid, "Unpriced " + wallet_unpriced)})},
        {"account_operating", json::array({account_operating_row(wallet_a, cur_eq),
                                            account_operating_row(wallet_b, cur_eq),
                                            account_operating_row(wallet_unpriced, cur_no_rate)})},
        // capital-assets: B — актив вне чистого капитала, чтобы все три итога
        // ветки «только итоги» сверялись с детализацией не нулями.
        {"account_asset", json::array({json{{"uuid", wallet_b},
                                             {"asset_type", 2},
                                             {"include_in_net_worth", false},
                                             {"comment", ""}}})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2027-03-02T10:00:00", category_uuid, wallet_a, "40.00"),
             operation_row(uuidgen::v4(), "2027-03-02T11:00:00", category_uuid, wallet_b, "2.50"),
             operation_row(uuidgen::v4(), "2027-03-03T10:00:00", category_uuid, wallet_unpriced, "25.00"),
         })},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto eq_currency = reports_repo::resolve_eq_currency(*reports_db, iso_eq, identity.ledger_id);
    REQUIRE(eq_currency.has_value());

    auto detailed = reports_repo::balance_series(*reports_db, identity.ledger_id, "2027-03-01", "2027-03-03",
                                                  "day", *eq_currency, true);
    auto totals = reports_repo::balance_series(*reports_db, identity.ledger_id, "2027-03-01", "2027-03-03",
                                                "day", *eq_currency, false);

    REQUIRE(totals.buckets.size() == static_cast<size_t>(3));
    REQUIRE(detailed.buckets.size() == totals.buckets.size());
    for (size_t i = 0; i < totals.buckets.size(); ++i) {
        CHECK(!totals.buckets[i].contains("accounts"));
        CHECK_EQ(totals.buckets[i].at("bucket_start").get<std::string>(),
                 detailed.buckets[i].at("bucket_start").get<std::string>());
        CHECK_EQ(totals.buckets[i].at("bucket_end").get<std::string>(),
                 detailed.buckets[i].at("bucket_end").get<std::string>());
        CHECK_EQ(totals.buckets[i].at("total_eq").get<std::string>(),
                 detailed.buckets[i].at("total_eq").get<std::string>());
        CHECK_EQ(totals.buckets[i].at("incomplete").get<bool>(),
                 detailed.buckets[i].at("incomplete").get<bool>());
        for (const char* k : {"capital_eq", "net_worth_eq"}) {
            CHECK_EQ(totals.buckets[i].at(k).get<std::string>(), detailed.buckets[i].at(k).get<std::string>());
        }
        CHECK_EQ(totals.buckets[i].at("capital_incomplete").get<bool>(),
                 detailed.buckets[i].at("capital_incomplete").get<bool>());
    }
    CHECK_EQ(totals.buckets[0].at("total_eq").get<std::string>(), std::string("0")); // до первой операции
    CHECK(!totals.buckets[0].at("incomplete").get<bool>());
    CHECK_EQ(totals.buckets[1].at("total_eq").get<std::string>(), std::string("40.00")); // B — актив
    CHECK_EQ(totals.buckets[1].at("capital_eq").get<std::string>(), std::string("2.50"));
    CHECK_EQ(totals.buckets[1].at("net_worth_eq").get<std::string>(), std::string("40.00"));
    CHECK(!totals.buckets[1].at("incomplete").get<bool>());
    CHECK(totals.buckets[2].at("incomplete").get<bool>()); // wallet_unpriced — курса к нему нет
}

TEST(list_currencies_includes_ledger_currency) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db); // .sha256_hex — сверить с icon_sha256 ответа, не просто "похоже на hex"
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "US" + uuidgen::v4().substr(0, 1); // уникальный ISO-код — таблица общая между тестами

    json changes = {{"currency", json::array({currency_row(currency_uuid, icon.uuid, iso)})}};
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto result = reports_repo::list_currencies(*reports_db, identity.ledger_id);
    bool found = false;
    for (const auto& row : result.currencies) {
        if (row.at("uuid").get<std::string>() != currency_uuid) continue;
        CHECK_EQ(row.at("iso_code").get<std::string>(), iso);
        CHECK_EQ(row.at("name").get<std::string>(), iso);   // currency_row() выше кладёт iso и в name, и в symbol
        CHECK_EQ(row.at("symbol").get<std::string>(), iso);
        CHECK_EQ(row.at("icon_sha256").get<std::string>(), icon.sha256_hex); // "флаг страны" в выпадающем списке, по прямому запросу
        // currency-ui-tweaks: своя валюта леджера, флаг по умолчанию (DEFAULT true).
        CHECK(row.at("is_own").is_boolean());
        CHECK(row.at("is_own").get<bool>());
        CHECK(row.at("show_in_equivalent").is_boolean());
        CHECK(row.at("show_in_equivalent").get<bool>());
        found = true;
    }
    CHECK(found);
}

// currency-ui-tweaks, «Поля для формы правки»: icon_uuid — uuid иконки, а не
// только её sha256; precision — JSON-число. Точность 3, не 2: значение по
// умолчанию колонки не должно случайно совпасть с ожидаемым.
TEST(list_currencies_returns_icon_uuid_and_numeric_precision) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "KZ" + uuidgen::v4().substr(0, 6);
    json cur = currency_row(currency_uuid, icon_uuid, iso);
    cur["precision"] = 3;

    auto push = sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(),
                                      json{{"currency", json::array({cur})}});
    REQUIRE(push.http_status == 200);

    auto result = reports_repo::list_currencies(*reports_db, identity.ledger_id);
    bool found = false;
    for (const auto& row : result.currencies) {
        if (row.at("uuid").get<std::string>() != currency_uuid) continue;
        CHECK_EQ(row.at("icon_uuid").get<std::string>(), icon_uuid);
        REQUIRE(row.at("precision").is_number_integer());
        CHECK_EQ(row.at("precision").get<int>(), 3);
        found = true;
    }
    CHECK(found);
}

// currency-ui-tweaks, «Валюта скрыта из эквивалента»: снятый флаг не убирает
// валюту из справочника — фильтрует клиент.
TEST(list_currencies_keeps_currency_hidden_from_equivalent) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "BT" + uuidgen::v4().substr(0, 6);
    json cur = currency_row(currency_uuid, icon_uuid, iso);
    cur["show_in_equivalent"] = false;

    auto push = sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(),
                                      json{{"currency", json::array({cur})}});
    REQUIRE(push.http_status == 200);

    auto result = reports_repo::list_currencies(*reports_db, identity.ledger_id);
    bool found = false;
    for (const auto& row : result.currencies) {
        if (row.at("uuid").get<std::string>() != currency_uuid) continue;
        CHECK(!row.at("show_in_equivalent").get<bool>());
        CHECK(row.at("is_own").get<bool>());
        found = true;
    }
    CHECK(found);
}

// currency-ui-tweaks, «Своя валюта перекрывает общую»: общая с флагом true,
// своя с тем же кодом и false — в списке одна строка, своя, с её флагом.
TEST(list_currencies_own_currency_overrides_reference_with_its_flag) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    std::string icon_uuid = fixture::make_reference_icon(*db);
    std::string iso = "OV" + uuidgen::v4().substr(0, 6);
    std::string reference_uuid = fixture::make_reference_currency(*db, iso, icon_uuid, true);
    std::string own_uuid = uuidgen::v4();
    json cur = currency_row(own_uuid, icon_uuid, iso);
    cur["show_in_equivalent"] = false;

    auto push = sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(),
                                      json{{"currency", json::array({cur})}});
    REQUIRE(push.http_status == 200);

    auto result = reports_repo::list_currencies(*reports_db, identity.ledger_id);
    int with_iso = 0;
    for (const auto& row : result.currencies) {
        if (row.at("iso_code").get<std::string>() != iso) continue;
        ++with_iso;
        CHECK_EQ(row.at("uuid").get<std::string>(), own_uuid);
        CHECK(row.at("is_own").get<bool>());
        CHECK(!row.at("show_in_equivalent").get<bool>());
    }
    CHECK_EQ(with_iso, 1);

    // Без леджера та же общая валюта видна сама — общая, с флагом true.
    auto reference_only = reports_repo::list_currencies(*reports_db, "");
    bool found_reference = false;
    for (const auto& row : reference_only.currencies) {
        if (row.at("uuid").get<std::string>() != reference_uuid) continue;
        CHECK(!row.at("is_own").get<bool>());
        CHECK(row.at("show_in_equivalent").get<bool>());
        found_reference = true;
    }
    CHECK(found_reference);
}

// Валюта из предыдущего теста — СВОЯ этого леджера (push_batch без явного
// ledger_id в currency_row кладёт её именно так, см. sync_repo.hpp), значит
// чужому леджеру (и тем более пользователю вовсе без леджера) не видна.
//
// НЕ проверяем, что список глобальных (ledger_id IS NULL) валют вообще
// пуст — база (wallet_api_it) общая с wallet-api/tests_integration (тот же
// IT_DB_NAME в обоих run_local.sh, не совпадение), а его
// test_currency_pairs.cpp намеренно заводит СВОИ reference-валюты напрямую
// SQL (fixture.hpp::make_currency с пустым ledger_id) и никогда не чистит
// за собой (оба сьюта рассчитаны на повторные прогоны без очистки) — такая
// проверка стала бы ложно падать всякий раз, как кто-то прогонит тесты
// wallet-api на этой же базе. Реальный инвариант, который стоит защищать —
// что валюта КОНКРЕТНОГО леджера в этот список не протекает (см. `WHERE
// c.ledger_id IS NULL` в reports_repo.cpp::list_currencies).
TEST(list_currencies_without_ledger_returns_only_reference) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "ZZ" + uuidgen::v4().substr(0, 1); // своя, ledger_id этого identity — уникальный ISO-код

    json changes = {{"currency", json::array({currency_row(currency_uuid, icon.uuid, iso)})}};
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto result = reports_repo::list_currencies(*reports_db, "");
    for (const auto& row : result.currencies) {
        CHECK(row.at("uuid").get<std::string>() != currency_uuid);
        // currency-ui-tweaks, «Нет леджера»: только общий справочник — у всех is_own: false.
        CHECK(row.at("is_own").is_boolean());
        CHECK(!row.at("is_own").get<bool>());
    }
}

// Справочник для Telegram-бота (org/docs/TODO_telegram_bot.md, фаза 1, п. 7):
// кошелёк/категория определяется по account_operating, а не по account.kind —
// account_row() выше ставит kind=0 даже кошелькам, и это ровно тот случай,
// который тест обязан пережить. Архивная категория видна только с
// include_archived=true; категория без единой операции — видна всегда (в
// этом и смысл эндпоинта в отличие от expense-structure).
TEST(list_accounts_splits_by_account_operating_and_filters_archived) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();

    auto icon = fixture::make_reference_icon_with_sha256(*db);
    std::string currency_uuid = uuidgen::v4();
    std::string iso = "AC" + uuidgen::v4().substr(0, 1);
    std::string wallet = uuidgen::v4();
    std::string category = uuidgen::v4();
    std::string archived = uuidgen::v4();

    json archived_row = account_row(archived, icon.uuid, "Archived " + archived);
    archived_row["is_archived"] = true;
    json changes = {
        {"currency", json::array({currency_row(currency_uuid, icon.uuid, iso)})},
        {"account", json::array({account_row(wallet, icon.uuid, "Wallet " + wallet),
                                  account_row(category, icon.uuid, "Cat " + category), archived_row})},
        {"account_operating", json::array({account_operating_row(wallet, currency_uuid)})},
    };
    auto push =
        sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);

    auto all = reports_repo::list_accounts(*reports_db, identity.ledger_id,
                                           reports_repo::AccountKindFilter::Any, false);
    REQUIRE(all.accounts.size() == static_cast<size_t>(2));
    // Кошельки первыми (ORDER BY kind, name).
    const auto& w = all.accounts[0];
    CHECK_EQ(w.at("uuid").get<std::string>(), wallet);
    CHECK_EQ(w.at("kind").get<std::string>(), std::string("wallet"));
    CHECK_EQ(w.at("currency_uuid").get<std::string>(), currency_uuid);
    CHECK_EQ(w.at("iso_code").get<std::string>(), iso);
    CHECK_EQ(w.at("precision").get<std::string>(), std::string("2"));
    CHECK_EQ(w.at("icon_sha256").get<std::string>(), icon.sha256_hex);
    CHECK_EQ(w.at("system_kind").get<int>(), 0);
    CHECK(!w.at("is_archived").get<bool>());
    CHECK(w.at("asset_type").is_null()); // capital-assets: ликвидный кошелёк
    CHECK(w.at("include_in_net_worth") == json(true));
    const auto& c = all.accounts[1];
    CHECK_EQ(c.at("uuid").get<std::string>(), category);
    CHECK_EQ(c.at("kind").get<std::string>(), std::string("category"));
    CHECK_EQ(c.at("currency_uuid").get<std::string>(), std::string(""));
    CHECK(c.at("asset_type").is_null());
    CHECK(c.at("include_in_net_worth").is_null());

    auto wallets = reports_repo::list_accounts(*reports_db, identity.ledger_id,
                                               reports_repo::AccountKindFilter::Wallet, false);
    REQUIRE(wallets.accounts.size() == static_cast<size_t>(1));
    CHECK_EQ(wallets.accounts[0].at("uuid").get<std::string>(), wallet);

    auto categories = reports_repo::list_accounts(*reports_db, identity.ledger_id,
                                                  reports_repo::AccountKindFilter::Category, true);
    REQUIRE(categories.accounts.size() == static_cast<size_t>(2));
    bool found_archived = false;
    for (const auto& row : categories.accounts) {
        CHECK_EQ(row.at("kind").get<std::string>(), std::string("category"));
        if (row.at("uuid").get<std::string>() == archived) {
            CHECK(row.at("is_archived").get<bool>());
            found_archived = true;
        }
    }
    CHECK(found_archived);
}

// ---------------------------------------------------------------------------
// Аналитика по категориям (change add-category-month-analytics, wallet-api 000030):
// op_count в структуре, матрица «категория × месяц», исключение категорий в ленте.
// ---------------------------------------------------------------------------

namespace {

// Леджер с двумя валютами (EUR -> USD по 1.10 с 2024-01-01), счётом Transfer
// (system_kind 1) и произвольными категориями. Эквивалент — USD.
struct AnalyticsLedger {
    fixture::TestIdentity identity;
    std::string icon_uuid;
    std::string usd, eur, iso_usd, pair_uuid;
    std::string w_usd, w_eur, transfer;
    std::string eq_currency;
};

void push_analytics(db::Database& db, db::Database& reports_db, AnalyticsLedger& l,
                    const std::vector<std::pair<std::string, std::string>>& categories, const json& operations,
                    const json& links) {
    std::string iso_eur = "AE" + uuidgen::v4().substr(0, 6);
    json accounts = json::array({account_row(l.w_usd, l.icon_uuid, "USD wallet " + l.w_usd),
                                 account_row(l.w_eur, l.icon_uuid, "EUR wallet " + l.w_eur),
                                 json{{"uuid", l.transfer}, {"name", "Transfer"}, {"comment", ""},
                                      {"icon_uuid", l.icon_uuid}, {"is_archived", false},
                                      {"system_kind", 1}, {"kind", 0}}});
    for (const auto& [uuid, name] : categories) accounts.push_back(account_row(uuid, l.icon_uuid, name));
    json changes = {
        {"currency", json::array({currency_row(l.usd, l.icon_uuid, l.iso_usd),
                                   currency_row(l.eur, l.icon_uuid, iso_eur)})},
        {"currency_pair", json::array({currency_pair_row(l.pair_uuid, iso_eur + l.iso_usd, l.eur, l.usd)})},
        {"currency_pair_rate",
         json::array({currency_pair_rate_row(uuidgen::v4(), l.pair_uuid, "2024-01-01", "1.10")})},
        {"account", accounts},
        {"account_operating", json::array({account_operating_row(l.w_usd, l.usd),
                                            account_operating_row(l.w_eur, l.eur)})},
        {"operation", operations},
        {"operation_link", links},
    };
    auto push = sync_repo::push_batch(db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);
    auto eq = reports_repo::resolve_eq_currency(reports_db, l.iso_usd, l.identity.ledger_id);
    REQUIRE(eq.has_value());
    l.eq_currency = *eq;
}

AnalyticsLedger new_analytics_ledger(db::Database& db) {
    AnalyticsLedger l;
    l.identity = fixture::make_identity(db);
    l.icon_uuid = fixture::make_reference_icon(db);
    l.usd = uuidgen::v4();
    l.eur = uuidgen::v4();
    l.iso_usd = "AU" + uuidgen::v4().substr(0, 6);
    l.pair_uuid = uuidgen::v4();
    l.w_usd = uuidgen::v4();
    l.w_eur = uuidgen::v4();
    l.transfer = uuidgen::v4();
    return l;
}

// Перевод USD -> EUR через Transfer: две ноги и связь type = 0.
void add_transfer(const AnalyticsLedger& l, const std::string& date, const std::string& usd_out,
                  const std::string& eur_in, json& operations, json& links) {
    std::string main = uuidgen::v4(), leg = uuidgen::v4();
    operations.push_back(operation_row(main, date, l.w_usd, l.transfer, usd_out));
    operations.push_back(operation_row(leg, date, l.transfer, l.w_eur, eur_in));
    links.push_back(operation_link_row(uuidgen::v4(), main, leg, 0));
}

const json* find_by_uuid(const json& rows, const std::string& uuid) {
    for (const auto& r : rows) {
        if (r.at("account_uuid").get<std::string>() == uuid) return &r;
    }
    return nullptr;
}

} // namespace

// op_count у строк структуры: три покупки — 3; категория только из периода
// сравнения — 0; Transfer — число переводов, чья разница вошла в сумму.
TEST(expense_structure_op_count) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = new_analytics_ledger(*db);
    std::string alcohol = uuidgen::v4(), only_cmp = uuidgen::v4();

    json ops = json::array({
        operation_row(uuidgen::v4(), "2026-06-02T10:00:00", l.w_usd, alcohol, "10.00"),
        operation_row(uuidgen::v4(), "2026-06-12T10:00:00", l.w_usd, alcohol, "20.00"),
        operation_row(uuidgen::v4(), "2026-06-22T10:00:00", l.w_usd, alcohol, "5.00"),
        operation_row(uuidgen::v4(), "2026-05-10T10:00:00", l.w_usd, only_cmp, "7.00"),
    });
    json links = json::array();
    add_transfer(l, "2026-06-15T10:00:00", "100.00", "80.00", ops, links); // 88 USD — потеря 12
    add_transfer(l, "2026-06-16T10:00:00", "50.00", "40.00", ops, links);  // 44 USD — потеря 6
    push_analytics(*db, *reports_db, l, {{alcohol, "Alcohol"}, {only_cmp, "Only cmp"}}, ops, links);

    auto r = reports_repo::expense_structure_for_period(*reports_db, l.identity.ledger_id, "2026-06-01T00:00:00",
                                                        "2026-06-30T23:59:59", l.eq_currency,
                                                        std::string("2026-05-01T00:00:00"),
                                                        std::string("2026-05-31T23:59:59"));
    REQUIRE(r.categories.size() == static_cast<size_t>(3));
    const json* a = find_by_uuid(r.categories, alcohol);
    const json* t = find_by_uuid(r.categories, l.transfer);
    const json* c = find_by_uuid(r.categories, only_cmp);
    REQUIRE(a != nullptr);
    REQUIRE(t != nullptr);
    REQUIRE(c != nullptr);
    CHECK_EQ(a->at("amount_eq").get<std::string>(), std::string("35.00"));
    CHECK(a->at("op_count").is_number_integer());
    CHECK_EQ(a->at("op_count").get<long long>(), 3LL);
    CHECK_EQ(t->at("amount_eq").get<std::string>(), std::string("18.00"));
    CHECK_EQ(t->at("op_count").get<long long>(), 2LL);
    CHECK_EQ(c->at("amount_eq").get<std::string>(), std::string("0"));
    CHECK(c->at("op_count").is_number_integer());
    CHECK_EQ(c->at("op_count").get<long long>(), 0LL);
}

// Матрица за период сходится со структурой за тот же период: итог и число
// операций каждой категории, общий итог; Transfer — net по месяцам (потеря
// 3 в июне, выигрыш 1 в июле), а не обе ноги.
TEST(expense_by_category_by_month_matches_structure) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = new_analytics_ledger(*db);
    std::string food = uuidgen::v4(), alcohol = uuidgen::v4(), cafe = uuidgen::v4();

    json ops = json::array({
        operation_row(uuidgen::v4(), "2026-06-05T10:00:00", l.w_usd, food, "60.00"),
        operation_row(uuidgen::v4(), "2026-06-25T10:00:00", l.w_usd, food, "40.00"),
        operation_row(uuidgen::v4(), "2026-07-05T10:00:00", l.w_usd, food, "150.00"),
        // 1.15 EUR * 1.10 = 1.265 USD — v_operation_eq округляет каждую операцию до 1.27.
        operation_row(uuidgen::v4(), "2026-06-07T10:00:00", l.w_eur, alcohol, "1.15"),
        operation_row(uuidgen::v4(), "2026-08-07T10:00:00", l.w_eur, alcohol, "1.15"),
        operation_row(uuidgen::v4(), "2026-08-08T10:00:00", l.w_usd, cafe, "12.34"),
    });
    json links = json::array();
    add_transfer(l, "2026-06-15T10:00:00", "91.00", "80.00", ops, links); // 88 USD — потеря 3
    add_transfer(l, "2026-07-15T10:00:00", "43.00", "40.00", ops, links); // 44 USD — выигрыш 1
    push_analytics(*db, *reports_db, l, {{food, "Продукты"}, {alcohol, "Alcohol"}, {cafe, "Кафе"}}, ops, links);

    const char* from = "2026-06-01T00:00:00";
    const char* to = "2026-08-31T23:59:59";
    auto m = reports_repo::expense_by_category_by_month(*reports_db, l.identity.ledger_id, from, to, l.eq_currency);
    auto s = reports_repo::expense_structure_for_period(*reports_db, l.identity.ledger_id, from, to, l.eq_currency,
                                                        std::nullopt, std::nullopt);

    CHECK(m.months == json::array({"2026-06-01", "2026-07-01", "2026-08-01"}));
    REQUIRE(m.categories.size() == s.categories.size());
    REQUIRE(m.categories.size() == static_cast<size_t>(4));
    for (const auto& sc : s.categories) {
        const json* mc = find_by_uuid(m.categories, sc.at("account_uuid").get<std::string>());
        REQUIRE(mc != nullptr);
        CHECK_EQ(mc->at("amount_eq").get<std::string>(), sc.at("amount_eq").get<std::string>());
        CHECK_EQ(mc->at("op_count").get<long long>(), sc.at("op_count").get<long long>());
    }
    CHECK_EQ(m.total_eq, s.total_eq);
    CHECK_EQ(m.total_eq, std::string("266.88")); // 250 + 2.54 + 12.34 + 2
    CHECK(!m.incomplete);

    const json* f = find_by_uuid(m.categories, food);
    REQUIRE(f != nullptr);
    CHECK_EQ(f->at("amount_eq").get<std::string>(), std::string("250.00"));
    CHECK_EQ(f->at("op_count").get<long long>(), 3LL);
    CHECK_EQ(f->at("account_name").get<std::string>(), std::string("Продукты"));
    REQUIRE(f->at("months").size() == static_cast<size_t>(2));
    CHECK_EQ(f->at("months")[0].at("month_start").get<std::string>(), std::string("2026-06-01"));
    CHECK_EQ(f->at("months")[0].at("amount_eq").get<std::string>(), std::string("100.00"));
    CHECK_EQ(f->at("months")[0].at("op_count").get<long long>(), 2LL);
    CHECK_EQ(f->at("months")[1].at("month_start").get<std::string>(), std::string("2026-07-01"));
    CHECK_EQ(f->at("months")[1].at("amount_eq").get<std::string>(), std::string("150.00"));
    CHECK_EQ(f->at("months")[1].at("incomplete").get<bool>(), false);

    const json* a = find_by_uuid(m.categories, alcohol);
    REQUIRE(a != nullptr);
    CHECK_EQ(a->at("amount_eq").get<std::string>(), std::string("2.54"));
    REQUIRE(a->at("months").size() == static_cast<size_t>(2));
    CHECK_EQ(a->at("months")[0].at("amount_eq").get<std::string>(), std::string("1.27"));

    const json* t = find_by_uuid(m.categories, l.transfer);
    REQUIRE(t != nullptr);
    CHECK_EQ(t->at("amount_eq").get<std::string>(), std::string("2.00"));
    CHECK_EQ(t->at("op_count").get<long long>(), 2LL);
    REQUIRE(t->at("months").size() == static_cast<size_t>(2));
    CHECK_EQ(t->at("months")[0].at("month_start").get<std::string>(), std::string("2026-06-01"));
    CHECK_EQ(t->at("months")[0].at("amount_eq").get<std::string>(), std::string("3.00"));
    CHECK_EQ(t->at("months")[0].at("op_count").get<long long>(), 1LL);
    CHECK_EQ(t->at("months")[1].at("amount_eq").get<std::string>(), std::string("-1.00"));

    // Порядок — по итогу категории убыванию.
    CHECK_EQ(m.categories[0].at("account_uuid").get<std::string>(), food);
}

// Пустой месяц есть в списке месяцев и отсутствует у категорий; неполный
// первый месяц считается только с from (25 месяцев 2024-11..2026-11).
TEST(expense_by_category_by_month_months) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = new_analytics_ledger(*db);
    std::string cat = uuidgen::v4();

    json ops = json::array({
        operation_row(uuidgen::v4(), "2024-11-03T10:00:00", l.w_usd, cat, "1000.00"), // до from
        operation_row(uuidgen::v4(), "2024-11-10T10:00:00", l.w_usd, cat, "10.00"),
        operation_row(uuidgen::v4(), "2026-06-10T10:00:00", l.w_usd, cat, "20.00"),
        operation_row(uuidgen::v4(), "2026-08-10T10:00:00", l.w_usd, cat, "30.00"),
    });
    json links = json::array();
    push_analytics(*db, *reports_db, l, {{cat, "Cat"}}, ops, links);

    auto m = reports_repo::expense_by_category_by_month(*reports_db, l.identity.ledger_id, "2026-06-01T00:00:00",
                                                        "2026-08-31T23:59:59", l.eq_currency);
    CHECK(m.months == json::array({"2026-06-01", "2026-07-01", "2026-08-01"}));
    REQUIRE(m.categories.size() == static_cast<size_t>(1));
    const auto& months = m.categories[0].at("months");
    REQUIRE(months.size() == static_cast<size_t>(2));
    CHECK_EQ(months[0].at("month_start").get<std::string>(), std::string("2026-06-01"));
    CHECK_EQ(months[1].at("month_start").get<std::string>(), std::string("2026-08-01"));
    CHECK_EQ(m.total_eq, std::string("50.00"));

    auto w = reports_repo::expense_by_category_by_month(*reports_db, l.identity.ledger_id, "2024-11-05T00:00:00",
                                                        "2026-11-04T23:59:59", l.eq_currency);
    REQUIRE(w.months.size() == static_cast<size_t>(25));
    CHECK_EQ(w.months.front().get<std::string>(), std::string("2024-11-01"));
    CHECK_EQ(w.months.back().get<std::string>(), std::string("2026-11-01"));
    REQUIRE(w.categories.size() == static_cast<size_t>(1));
    const auto& wm = w.categories[0].at("months");
    REQUIRE(wm.size() == static_cast<size_t>(3));
    CHECK_EQ(wm[0].at("month_start").get<std::string>(), std::string("2024-11-01"));
    CHECK_EQ(wm[0].at("amount_eq").get<std::string>(), std::string("10.00"));
    CHECK_EQ(wm[0].at("op_count").get<long long>(), 1LL);
    CHECK_EQ(w.total_eq, std::string("60.00"));

    // Пустой период — пустые категории, итог "0".
    auto e = reports_repo::expense_by_category_by_month(*reports_db, l.identity.ledger_id, "2025-01-01T00:00:00",
                                                        "2025-01-31T23:59:59", l.eq_currency);
    CHECK(e.months == json::array({"2025-01-01"}));
    CHECK(e.categories.empty());
    CHECK_EQ(e.total_eq, std::string("0"));
    CHECK(!e.incomplete);
}

// Исключение категорий: ни в страницах, ни в итоге; прямой перевод остаётся;
// с wallet_uuid — по И; исключение категории дохода (сторона «откуда»);
// обход по курсору без пропусков и дублей.
TEST(operations_filter_exclude_categories) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_filter_ledger(*db, *reports_db);

    reports_repo::OperationsFilter f;
    f.exclude_category_uuids = {l.k_taxi, l.k_nanny};
    f.kind = "expense";
    auto rows = all_filtered_rows(*reports_db, l, f, 2);
    CHECK(sorted_uuids(rows) == sorted({l.e3, l.e4, l.e6, l.e7, l.u1}));
    auto r = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                                  FilterLedger::kTo, l.eq_currency, 2, "", "", f);
    CHECK(r.has_total);
    CHECK_EQ(as_number(r.total_eq), 320.0); // 120 + 80 + 70 + 50; u1 без курса
    CHECK_EQ(r.total_count, 5LL);
    CHECK(r.total_incomplete);

    f = {};
    f.exclude_category_uuids = {l.k_taxi}; // без kind — прямой перевод t1 остаётся
    CHECK(filtered_uuids(*reports_db, l, f) ==
          sorted({l.e3, l.e4, l.e5, l.i1, l.t1, l.e6, l.e7, l.u1}));

    f = {};
    f.wallet_uuids = {l.w_tinkoff};
    f.exclude_category_uuids = {l.k_food}; // И: «Тинькофф», кроме «Продуктов»
    CHECK(filtered_uuids(*reports_db, l, f) == sorted({l.e1, l.e3, l.e5, l.i1, l.t1}));

    f = {};
    f.exclude_category_uuids = {l.k_salary}; // категория на стороне «откуда»
    auto all = filtered_uuids(*reports_db, l, f);
    CHECK(std::find(all.begin(), all.end(), l.i1) == all.end());
    CHECK_EQ(all.size(), static_cast<size_t>(9));

    // Итог совпадает с суммой по всем страницам (лента и итог — одни условия).
    f = {};
    f.exclude_category_uuids = {l.k_cafe};
    f.kind = "expense";
    double sum = 0;
    for (const auto& row : all_filtered_rows(*reports_db, l, f, 3)) {
        const auto& eq = row.at("amount_eq").get<std::string>();
        if (!eq.empty()) sum += as_number(eq);
    }
    r = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, FilterLedger::kFrom,
                                             FilterLedger::kTo, l.eq_currency, 1000, "", "", f);
    CHECK_EQ(as_number(r.total_eq), sum);
    CHECK_EQ(r.total_count, static_cast<long long>(r.rows.size()));
}

TEST(operations_filter_exclude_parse_texts) {
    reports_repo::OperationsFilter f;
    std::string u = uuidgen::v4();

    CHECK(!reports_repo::parse_operations_filter("", "", "", "", "", f).has_value());
    CHECK(!f.any()); // пустое значение = без фильтра

    CHECK_EQ(reports_repo::parse_operations_filter("", "abc", "", "", "", f).value_or(""),
             std::string("exclude_category_uuid: некорректный uuid"));
    CHECK_EQ(reports_repo::parse_operations_filter("", u + ",", "", "", "", f).value_or(""),
             std::string("exclude_category_uuid: некорректный uuid"));

    std::string fifty = u;
    for (int i = 1; i < 50; ++i) fifty += "," + u;
    CHECK(!reports_repo::parse_operations_filter("", fifty, "", "", "", f).has_value());
    CHECK_EQ(f.exclude_category_uuids.size(), static_cast<size_t>(50));
    CHECK_EQ(reports_repo::parse_operations_filter("", fifty + "," + u, "", "", "", f).value_or(""),
             std::string("exclude_category_uuid: не больше 50 значений"));

    CHECK_EQ(reports_repo::parse_operations_filter(u, u, "", "", "", f).value_or(""),
             std::string("category_uuid и exclude_category_uuid нельзя задавать вместе"));

    CHECK(!reports_repo::parse_operations_filter("", u, u, "expense", "", f).has_value());
    CHECK(f.any());
    CHECK_EQ(f.exclude_category_uuids.size(), static_cast<size_t>(1));
    CHECK(f.category_uuids.empty());
}

// ---- capital-assets: активы, переоценка, отчёт «Капитал» ------------------
// Сценарии specs/* change capital-assets. Один леджер в одной валюте (она же
// эквивалент), суммы — числа из spec:
//   2026-01-10 Зарплата → Карта     20 000 000  (доход)
//   2026-02-01 Карта → Квартира     10 000 000  (покупка — перевод)
//   2026-03-01 Карта → Квартира      1 500 000  (ремонт — перевод)
//   2026-03-15 Переоценка → Квартира 2 000 000  (переоценка вверх, до периода)
//   2026-04-01 Карта → Машина        1 000 000  (покупка — перевод)
//   2026-05-01 Квартира → Переоценка   500 000  (переоценка вниз, в периоде)
//   2026-05-02 Карта → Продукты         10 000  (расход, суббота)
//   2026-06-01 Машина → Переоценка     300 000  (износ, в периоде)
// Квартира — asset_type 1, Машина — asset_type 2 и include_in_net_worth = false.

namespace {

json asset_row(const std::string& wallet_uuid, int asset_type, bool include_in_net_worth = true) {
    return json{{"uuid", wallet_uuid},
                {"asset_type", asset_type},
                {"include_in_net_worth", include_in_net_worth},
                {"comment", ""}};
}

json system_account_row(const std::string& uuid, const std::string& icon_uuid, const std::string& name,
                        int system_kind) {
    json row = account_row(uuid, icon_uuid, name);
    row["system_kind"] = system_kind;
    return row;
}

struct CapitalLedger {
    fixture::TestIdentity identity;
    std::string icon, cur, iso, eq;
    std::string card = uuidgen::v4(), flat = uuidgen::v4(), car = uuidgen::v4();
    std::string food = uuidgen::v4(), salary = uuidgen::v4(), reval = uuidgen::v4();
    std::string op_reval_up = uuidgen::v4(), op_reval_down = uuidgen::v4(), op_wear = uuidgen::v4();
    std::string op_buy = uuidgen::v4();
};

CapitalLedger seed_capital_ledger(db::Database& db, db::Database& reports_db) {
    CapitalLedger l;
    l.identity = fixture::make_identity(db);
    l.icon = fixture::make_reference_icon(db);
    l.cur = uuidgen::v4();
    l.iso = "CA" + uuidgen::v4().substr(0, 1);
    json changes = {
        {"currency", json::array({currency_row(l.cur, l.icon, l.iso)})},
        {"account", json::array({account_row(l.card, l.icon, "Карта " + l.card),
                                  account_row(l.flat, l.icon, "Квартира " + l.flat),
                                  account_row(l.car, l.icon, "Машина " + l.car),
                                  account_row(l.food, l.icon, "Продукты " + l.food),
                                  account_row(l.salary, l.icon, "Зарплата " + l.salary),
                                  system_account_row(l.reval, l.icon, "Переоценка " + l.reval, 3)})},
        {"account_operating", json::array({account_operating_row(l.card, l.cur),
                                            account_operating_row(l.flat, l.cur),
                                            account_operating_row(l.car, l.cur)})},
        {"account_asset", json::array({asset_row(l.flat, 1), asset_row(l.car, 2, false)})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-01-10T10:00:00", l.salary, l.card, "20000000.00"),
             operation_row(l.op_buy, "2026-02-01T10:00:00", l.card, l.flat, "10000000.00"),
             operation_row(uuidgen::v4(), "2026-03-01T10:00:00", l.card, l.flat, "1500000.00"),
             operation_row(l.op_reval_up, "2026-03-15T10:00:00", l.reval, l.flat, "2000000.00"),
             operation_row(uuidgen::v4(), "2026-04-01T10:00:00", l.card, l.car, "1000000.00"),
             operation_row(l.op_reval_down, "2026-05-01T10:00:00", l.flat, l.reval, "500000.00"),
             operation_row(uuidgen::v4(), "2026-05-02T10:00:00", l.card, l.food, "10000.00"),
             operation_row(l.op_wear, "2026-06-01T10:00:00", l.car, l.reval, "300000.00"),
         })},
    };
    auto push = sync_repo::push_batch(db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);
    auto eq = reports_repo::resolve_eq_currency(reports_db, l.iso, l.identity.ledger_id);
    REQUIRE(eq.has_value());
    l.eq = *eq;
    return l;
}

const json* find_by(const json& rows, const std::string& key, const std::string& uuid) {
    for (const auto& r : rows) {
        if (r.at(key).get<std::string>() == uuid) return &r;
    }
    return nullptr;
}

} // namespace

TEST(seed_asset_and_revaluation_category_via_push) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);
    auto res = reports_db->exec(
        "SELECT (SELECT count(*) FROM account_asset WHERE ledger_id = $1 AND deleted_at IS NULL), "
        "(SELECT system_kind FROM account WHERE ledger_id = $1 AND uuid = $2)",
        {l.identity.ledger_id, l.reval});
    CHECK_EQ(res.rows.at(0).fields.at(0), std::string("2"));
    CHECK_EQ(res.rows.at(0).fields.at(1), std::string("3"));
}

TEST(balance_splits_liquid_capital_and_net_worth) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);

    auto r = reports_repo::balance_to_date(*reports_db, l.identity.ledger_id, "2027-01-01", l.eq);
    CHECK_EQ(r.total_eq, std::string("7490000.00"));      // только Карта
    CHECK_EQ(r.capital_eq, std::string("13700000.00"));   // Квартира 13 000 000 + Машина 700 000
    CHECK_EQ(r.net_worth_eq, std::string("20490000.00")); // Машина вне чистого капитала
    CHECK(!r.incomplete);
    CHECK(!r.capital_incomplete);

    const json* flat = find_by(r.rows, "account_uuid", l.flat);
    const json* car = find_by(r.rows, "account_uuid", l.car);
    const json* card = find_by(r.rows, "account_uuid", l.card);
    REQUIRE(flat && car && card);
    CHECK(flat->at("asset_type") == json(1));
    CHECK(flat->at("include_in_net_worth") == json(true));
    CHECK(car->at("asset_type") == json(2));
    CHECK(car->at("include_in_net_worth") == json(false));
    CHECK(card->at("asset_type").is_null());
    CHECK(card->at("include_in_net_worth") == json(true));

    // Те же итоги в блоке /balance-at-dates и в бакете /balance-series (обе ветки).
    auto at = reports_repo::balances_at_dates(*reports_db, l.identity.ledger_id, {"2027-01-01"}, l.eq);
    REQUIRE(at.dates.size() == static_cast<size_t>(1));
    CHECK_EQ(at.dates[0].at("total_eq").get<std::string>(), std::string("7490000.00"));
    CHECK_EQ(at.dates[0].at("capital_eq").get<std::string>(), std::string("13700000.00"));
    CHECK_EQ(at.dates[0].at("net_worth_eq").get<std::string>(), std::string("20490000.00"));
    const json* at_flat = find_by(at.dates[0].at("accounts"), "account_uuid", l.flat);
    REQUIRE(at_flat != nullptr);
    CHECK(at_flat->at("asset_type") == json(1));

    for (bool with_accounts : {true, false}) {
        auto s = reports_repo::balance_series(*reports_db, l.identity.ledger_id, "2026-12-01", "2026-12-31",
                                              "month", l.eq, with_accounts);
        REQUIRE(s.buckets.size() == static_cast<size_t>(1));
        CHECK_EQ(s.buckets[0].at("total_eq").get<std::string>(), std::string("7490000.00"));
        CHECK_EQ(s.buckets[0].at("capital_eq").get<std::string>(), std::string("13700000.00"));
        CHECK_EQ(s.buckets[0].at("net_worth_eq").get<std::string>(), std::string("20490000.00"));
        CHECK(!s.buckets[0].at("capital_incomplete").get<bool>());
    }
}

TEST(balance_asset_without_rate_marks_only_capital_incomplete) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();
    std::string icon = fixture::make_reference_icon(*db);
    std::string cur = uuidgen::v4(), cur_no_rate = uuidgen::v4();
    std::string iso = "CB" + uuidgen::v4().substr(0, 1), iso_nr = "CN" + uuidgen::v4().substr(0, 1);
    std::string card = uuidgen::v4(), house = uuidgen::v4(), income = uuidgen::v4();
    json changes = {
        {"currency", json::array({currency_row(cur, icon, iso), currency_row(cur_no_rate, icon, iso_nr)})},
        {"account", json::array({account_row(card, icon, "Card " + card), account_row(house, icon, "House " + house),
                                  account_row(income, icon, "Inc " + income)})},
        {"account_operating",
         json::array({account_operating_row(card, cur), account_operating_row(house, cur_no_rate)})},
        {"account_asset", json::array({asset_row(house, 1)})},
        {"operation", json::array({operation_row(uuidgen::v4(), "2026-01-10T10:00:00", income, card, "100000.00"),
                                    operation_row(uuidgen::v4(), "2026-01-11T10:00:00", income, house, "5000000.00")})},
    };
    REQUIRE(sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes).http_status ==
            200);
    auto eq = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq.has_value());

    auto r = reports_repo::balance_to_date(*reports_db, identity.ledger_id, "2027-01-01", *eq);
    CHECK_EQ(r.total_eq, std::string("100000.00"));
    CHECK(!r.incomplete);
    CHECK(r.capital_incomplete);
    CHECK_EQ(r.capital_eq, std::string("0"));

    auto a = reports_repo::assets_for_period(*reports_db, identity.ledger_id, "2026-01-01", "2026-12-31 23:59:59", *eq);
    REQUIRE(a.rows.size() == static_cast<size_t>(1));
    CHECK(a.incomplete);
    CHECK_EQ(a.rows[0].at("value").get<std::string>(), std::string("5000000.00"));
    CHECK(a.rows[0].at("value_eq").is_null());
    CHECK(a.rows[0].at("invested_eq").is_null());
}

TEST(balance_unmarked_asset_returns_to_liquid_total) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);
    json tombstone = asset_row(l.car, 2, false);
    tombstone["deleted"] = true;
    REQUIRE(sync_repo::push_batch(*db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(),
                                  {{"account_asset", json::array({tombstone})}})
                .http_status == 200);

    auto r = reports_repo::balance_to_date(*reports_db, l.identity.ledger_id, "2027-01-01", l.eq);
    CHECK_EQ(r.total_eq, std::string("8190000.00")); // Карта + Машина
    CHECK_EQ(r.capital_eq, std::string("13000000.00"));
    const json* car = find_by(r.rows, "account_uuid", l.car);
    REQUIRE(car != nullptr);
    CHECK(car->at("asset_type").is_null());
    CHECK_EQ(car->at("balance").get<std::string>(), std::string("700000.00")); // баланс не тронут
}

TEST(operations_for_period_classifies_revaluation) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);

    auto ops = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, "2026-01-01",
                                                   "2026-12-31 23:59:59", l.eq, 100, "", "");
    const json* up = find_by(ops.rows, "operation_uuid", l.op_reval_up);
    const json* down = find_by(ops.rows, "operation_uuid", l.op_wear);
    const json* buy = find_by(ops.rows, "operation_uuid", l.op_buy);
    REQUIRE(up && down && buy);
    CHECK_EQ(up->at("kind").get<std::string>(), std::string("revaluation"));
    CHECK_EQ(up->at("category_uuid").get<std::string>(), l.reval);
    CHECK_EQ(up->at("wallet_uuid").get<std::string>(), l.flat);
    CHECK_EQ(up->at("to_uuid").get<std::string>(), l.flat);
    CHECK_EQ(down->at("kind").get<std::string>(), std::string("revaluation"));
    CHECK_EQ(down->at("from_uuid").get<std::string>(), l.car);
    CHECK_EQ(down->at("wallet_uuid").get<std::string>(), l.car);
    CHECK_EQ(buy->at("kind").get<std::string>(), std::string("transfer"));

    reports_repo::OperationsFilter f;
    f.kind = "revaluation";
    auto only = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, "2026-01-01",
                                                    "2026-12-31 23:59:59", l.eq, 100, "", "", f);
    CHECK_EQ(only.rows.size(), static_cast<size_t>(3));
}

TEST(income_and_expense_exclude_revaluation) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);
    const std::string from = "2026-01-01", to = "2026-12-31 23:59:59";

    auto inc = reports_repo::income_for_period(*reports_db, l.identity.ledger_id, from, to, l.eq);
    REQUIRE(inc.rows.size() == static_cast<size_t>(1));
    CHECK_EQ(inc.rows[0].at("account_uuid").get<std::string>(), l.salary);
    CHECK_EQ(inc.total_eq, std::string("20000000.00"));

    auto exp = reports_repo::expense_for_period(*reports_db, l.identity.ledger_id, from, to, l.eq);
    REQUIRE(exp.rows.size() == static_cast<size_t>(1));
    CHECK_EQ(exp.rows[0].at("account_uuid").get<std::string>(), l.food);
    CHECK_EQ(exp.total_eq, std::string("10000.00"));
}

TEST(income_expense_by_month_and_weekday_exclude_revaluation) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);

    auto m = reports_repo::income_expense_by_month(*reports_db, l.identity.ledger_id, "2026-01-01",
                                                   "2026-06-30 23:59:59", l.eq);
    REQUIRE(m.months.size() == static_cast<size_t>(6));
    CHECK_EQ(m.months[2].at("income_eq").get<std::string>(), std::string("0.00"));  // март: переоценка +2 000 000
    CHECK_EQ(m.months[4].at("expense_eq").get<std::string>(), std::string("10000.00")); // май: только Продукты
    CHECK_EQ(m.months[5].at("expense_eq").get<std::string>(), std::string("0.00"));     // июнь: только износ

    auto w = reports_repo::expense_by_weekday(*reports_db, l.identity.ledger_id, "2026-01-01",
                                              "2026-12-31 23:59:59", l.eq);
    REQUIRE(w.days.size() == static_cast<size_t>(7));
    for (const auto& d : w.days) {
        std::string expected = d.at("isodow").get<int>() == 6 ? "10000.00" : "0.00"; // 2026-05-02 — суббота
        CHECK_EQ(d.at("expense_eq").get<std::string>(), expected);
    }
}

TEST(expense_structure_and_biggest_moves_exclude_revaluation) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);

    auto s = reports_repo::expense_structure_for_period(*reports_db, l.identity.ledger_id, "2026-05-01",
                                                        "2026-06-30 23:59:59", l.eq, std::nullopt, std::nullopt);
    REQUIRE(s.categories.size() == static_cast<size_t>(1));
    CHECK_EQ(s.categories[0].at("account_uuid").get<std::string>(), l.food);
    CHECK(s.categories[0].at("pct") == json("100.0")); // износ машины в долю не входит

    auto mv = reports_repo::expense_biggest_moves(*reports_db, l.identity.ledger_id, "2026-05-01",
                                                  "2026-06-30 23:59:59", "2026-03-01", "2026-04-30 23:59:59", l.eq, 10);
    for (const auto& r : mv.rows) CHECK(r.at("account_uuid").get<std::string>() != l.reval);
    CHECK_EQ(mv.rows.size(), static_cast<size_t>(1));
}

TEST(wallet_turnover_excludes_revaluation_and_marks_asset) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);

    auto t = reports_repo::wallet_turnover_for_period(*reports_db, l.identity.ledger_id, "2026-01-01",
                                                      "2026-12-31 23:59:59", l.eq);
    const json* flat = find_by(t.rows, "account_uuid", l.flat);
    const json* car = find_by(t.rows, "account_uuid", l.car);
    const json* card = find_by(t.rows, "account_uuid", l.card);
    REQUIRE(flat && car && card);
    CHECK_EQ(flat->at("turnover_in").get<std::string>(), std::string("11500000.00")); // без переоценки +2 000 000
    CHECK_EQ(flat->at("turnover_out").get<std::string>(), std::string("0.00"));       // без переоценки −500 000
    CHECK_EQ(car->at("turnover_out").get<std::string>(), std::string("0.00"));        // без износа
    CHECK(flat->at("asset_type") == json(1));
    CHECK(car->at("asset_type") == json(2));
    CHECK(card->at("asset_type").is_null());
}

TEST(list_accounts_marks_assets) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);

    auto a = reports_repo::list_accounts(*reports_db, l.identity.ledger_id, reports_repo::AccountKindFilter::Any, false);
    const json* flat = find_by(a.accounts, "uuid", l.flat);
    const json* car = find_by(a.accounts, "uuid", l.car);
    const json* card = find_by(a.accounts, "uuid", l.card);
    const json* reval = find_by(a.accounts, "uuid", l.reval);
    REQUIRE(flat && car && card && reval);
    CHECK_EQ(flat->at("kind").get<std::string>(), std::string("wallet"));
    CHECK(flat->at("asset_type") == json(1));
    CHECK(car->at("include_in_net_worth") == json(false));
    CHECK(card->at("asset_type").is_null());
    CHECK(card->at("include_in_net_worth") == json(true));
    CHECK_EQ(reval->at("kind").get<std::string>(), std::string("category"));
    CHECK(reval->at("system_kind") == json(3));
    CHECK(reval->at("asset_type").is_null());
    CHECK(reval->at("include_in_net_worth").is_null());
}

TEST(assets_for_period_invested_revaluation_value_and_return) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);

    auto a = reports_repo::assets_for_period(*reports_db, l.identity.ledger_id, "2026-04-01", "2026-12-31 23:59:59", l.eq);
    REQUIRE(a.rows.size() == static_cast<size_t>(2)); // Карта — не актив
    // Порядок — по оценке в эквиваленте убыв.
    CHECK_EQ(a.rows[0].at("account_uuid").get<std::string>(), l.flat);
    CHECK_EQ(a.rows[1].at("account_uuid").get<std::string>(), l.car);

    const json& flat = a.rows[0];
    CHECK_EQ(flat.at("invested").get<std::string>(), std::string("11500000.00"));
    CHECK_EQ(flat.at("revaluation_total").get<std::string>(), std::string("1500000.00"));
    CHECK_EQ(flat.at("revaluation_period").get<std::string>(), std::string("-500000.00"));
    CHECK_EQ(flat.at("value").get<std::string>(), std::string("13000000.00"));
    CHECK(flat.at("return_pct") == json("13.0"));
    CHECK(flat.at("asset_type") == json(1));

    const json& car = a.rows[1];
    CHECK_EQ(car.at("invested").get<std::string>(), std::string("1000000.00"));
    CHECK_EQ(car.at("value").get<std::string>(), std::string("700000.00"));
    CHECK(car.at("return_pct") == json("-30.0"));
    CHECK(car.at("include_in_net_worth") == json(false));

    CHECK_EQ(a.value_eq, std::string("13700000.00"));
    CHECK_EQ(a.invested_eq, std::string("12500000.00"));
    CHECK_EQ(a.revaluation_total_eq, std::string("1200000.00"));
    CHECK_EQ(a.revaluation_period_eq, std::string("-800000.00"));
    CHECK(!a.incomplete);

    // Оценка на конец отчёта = capital_eq остатков на следующий день.
    auto b = reports_repo::balance_to_date(*reports_db, l.identity.ledger_id, "2027-01-01", l.eq);
    CHECK_EQ(b.capital_eq, a.value_eq);
}

TEST(assets_for_period_sale_idle_asset_and_nothing_invested) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_capital_ledger(*db, *reports_db);
    std::string idle = uuidgen::v4();
    json idle_account = account_row(idle, l.icon, "Гараж " + idle);
    json changes = {
        {"account", json::array({idle_account})},
        {"account_operating", json::array({account_operating_row(idle, l.cur)})},
        {"account_asset", json::array({asset_row(idle, 3)})},
        // Продажа части квартиры и полный вывод машины.
        {"operation", json::array({operation_row(uuidgen::v4(), "2026-07-01T10:00:00", l.flat, l.card, "3000000.00"),
                                    operation_row(uuidgen::v4(), "2026-07-02T10:00:00", l.car, l.card, "700000.00")})},
    };
    REQUIRE(sync_repo::push_batch(*db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(), changes)
                .http_status == 200);

    auto a = reports_repo::assets_for_period(*reports_db, l.identity.ledger_id, "2026-04-01", "2026-12-31 23:59:59", l.eq);
    REQUIRE(a.rows.size() == static_cast<size_t>(3));
    const json* flat = find_by(a.rows, "account_uuid", l.flat);
    const json* car = find_by(a.rows, "account_uuid", l.car);
    const json* garage = find_by(a.rows, "account_uuid", idle);
    REQUIRE(flat && car && garage);
    CHECK_EQ(flat->at("invested").get<std::string>(), std::string("8500000.00")); // 11 500 000 − 3 000 000
    // Машина выведена целиком: оценка 0, переоценка −300 000 (износ), значит
    // invested = value − revaluation_total = 0 − (−300 000) = 300 000 —
    // столько из вложенного «съел» износ.
    CHECK_EQ(car->at("value").get<std::string>(), std::string("0.00"));
    CHECK_EQ(car->at("invested").get<std::string>(), std::string("300000.00"));
    // Актив без движения — нули в точности валюты и пустая доходность.
    CHECK_EQ(garage->at("value").get<std::string>(), std::string("0.00"));
    CHECK_EQ(garage->at("invested").get<std::string>(), std::string("0.00"));
    CHECK_EQ(garage->at("revaluation_total").get<std::string>(), std::string("0.00"));
    CHECK(garage->at("return_pct").is_null());
}

TEST(assets_for_period_without_ledger_assets_is_empty) {
    auto db = fixture::open();
    auto identity = fixture::make_identity(*db);
    auto reports_db = fixture::open_reports();
    std::string icon = fixture::make_reference_icon(*db);
    std::string cur = uuidgen::v4(), iso = "CE" + uuidgen::v4().substr(0, 1), card = uuidgen::v4();
    json changes = {{"currency", json::array({currency_row(cur, icon, iso)})},
                    {"account", json::array({account_row(card, icon, "Card " + card)})},
                    {"account_operating", json::array({account_operating_row(card, cur)})}};
    REQUIRE(sync_repo::push_batch(*db, identity.ledger_id, identity.device_id, uuidgen::v4(), changes).http_status ==
            200);
    auto eq = reports_repo::resolve_eq_currency(*reports_db, iso, identity.ledger_id);
    REQUIRE(eq.has_value());
    auto a = reports_repo::assets_for_period(*reports_db, identity.ledger_id, "2026-01-01", "2026-12-31", *eq);
    CHECK(a.rows.empty());
    CHECK_EQ(a.value_eq, std::string("0"));
    CHECK(!a.incomplete);
}

// ---- debts: долговые кошельки, контрагенты, отчёт «Долги» -----------------
// Сценарии specs/* change debts. Один леджер, эквивалент — валюта E (курс 1),
// F → E по 2 с 2026-01-01, у N курса нет. Период отчётов — март 2026.
//   2026-01-05 Зарплата → Карта (E)        5000
//   2026-01-06 Зарплата → Наличные F       1000  (= 2000 E)
//   2026-02-15 Зарплата → Коля · N           10  (долг без курса, архивный контрагент)
//   2026-03-05 Карта → Вася 1 (E)          1000  (выдано в долг)
//   2026-03-06 Карта → Вася 2 (E)           100  (второй заём, другой срок)
//   2026-03-07 Наличные F → Вася F           50  (= 100 E)
//   2026-03-08 Петя (E) → Карта              30  (взято в долг)
//   2026-03-20 Вася 1 → Карта               400  (частичный возврат)
//   2026-03-25 Вася 2 → Списание долгов      20  (прощение — расход)
//   2026-03-26 Вася 2 → Петя                  5  (перевод между долгами)
//   2026-04-05 Карта → Вася 1               999  (после периода)
// На 2026-04-01: Карта 4330, Наличные F 950 (= 1900); Вася 1 600, Вася 2 75,
// Вася F 50 (= 100), Петя −25, Коля 10 N (без курса).

namespace {

json counterparty_row(const std::string& uuid, const std::string& name, bool archived = false) {
    return json{{"uuid", uuid}, {"name", name}, {"comment", ""}, {"is_archived", archived}};
}

json debt_row(const std::string& wallet_uuid, const std::string& counterparty_uuid, const std::string& due_date) {
    json row = {{"uuid", wallet_uuid}, {"counterparty_uuid", counterparty_uuid}, {"comment", "заём"}};
    row["due_date"] = due_date.empty() ? json(nullptr) : json(due_date);
    return row;
}

struct DebtLedger {
    fixture::TestIdentity identity;
    std::string icon, e, f, n, iso_e, eq;
    std::string card = uuidgen::v4(), cash_f = uuidgen::v4(), salary = uuidgen::v4(), write_off = uuidgen::v4();
    std::string vasya = uuidgen::v4(), petya = uuidgen::v4(), kolya = uuidgen::v4();
    std::string vasya1 = uuidgen::v4(), vasya2 = uuidgen::v4(), vasya_f = uuidgen::v4();
    std::string petya_e = uuidgen::v4(), kolya_n = uuidgen::v4();
    std::string op_give = uuidgen::v4(), op_back = uuidgen::v4(), op_borrow = uuidgen::v4();
    std::string op_forgive = uuidgen::v4(), op_debt2debt = uuidgen::v4(), op_kolya = uuidgen::v4();
};

DebtLedger seed_debt_ledger(db::Database& db, db::Database& reports_db) {
    DebtLedger l;
    l.identity = fixture::make_identity(db);
    l.icon = fixture::make_reference_icon(db);
    l.e = uuidgen::v4();
    l.f = uuidgen::v4();
    l.n = uuidgen::v4();
    l.iso_e = "DE" + uuidgen::v4().substr(0, 1);
    std::string iso_f = "DF" + uuidgen::v4().substr(0, 1), iso_n = "DN" + uuidgen::v4().substr(0, 1);
    std::string pair = uuidgen::v4();
    json changes = {
        {"currency", json::array({currency_row(l.e, l.icon, l.iso_e), currency_row(l.f, l.icon, iso_f),
                                  currency_row(l.n, l.icon, iso_n)})},
        {"currency_pair", json::array({currency_pair_row(pair, iso_f + l.iso_e + pair, l.f, l.e)})},
        {"currency_pair_rate", json::array({currency_pair_rate_row(uuidgen::v4(), pair, "2026-01-01", "2")})},
        {"account", json::array({account_row(l.card, l.icon, "Карта " + l.card),
                                  account_row(l.cash_f, l.icon, "Наличные F " + l.cash_f),
                                  account_row(l.salary, l.icon, "Зарплата " + l.salary),
                                  account_row(l.write_off, l.icon, "Списание долгов " + l.write_off),
                                  account_row(l.vasya1, l.icon, "Вася 1 " + l.vasya1),
                                  account_row(l.vasya2, l.icon, "Вася 2 " + l.vasya2),
                                  account_row(l.vasya_f, l.icon, "Вася F " + l.vasya_f),
                                  account_row(l.petya_e, l.icon, "Петя " + l.petya_e),
                                  account_row(l.kolya_n, l.icon, "Коля N " + l.kolya_n)})},
        {"account_operating",
         json::array({account_operating_row(l.card, l.e), account_operating_row(l.cash_f, l.f),
                      account_operating_row(l.vasya1, l.e), account_operating_row(l.vasya2, l.e),
                      account_operating_row(l.vasya_f, l.f), account_operating_row(l.petya_e, l.e),
                      account_operating_row(l.kolya_n, l.n)})},
        {"counterparty", json::array({counterparty_row(l.vasya, "Вася " + l.vasya),
                                      counterparty_row(l.petya, "Петя " + l.petya),
                                      counterparty_row(l.kolya, "Коля " + l.kolya, /*archived=*/true)})},
        {"account_debt", json::array({debt_row(l.vasya1, l.vasya, "2026-12-31"), debt_row(l.vasya2, l.vasya, "2027-06-30"),
                                      debt_row(l.vasya_f, l.vasya, ""), debt_row(l.petya_e, l.petya, ""),
                                      debt_row(l.kolya_n, l.kolya, "")})},
        {"operation", json::array({
             operation_row(uuidgen::v4(), "2026-01-05T10:00:00", l.salary, l.card, "5000.00"),
             operation_row(uuidgen::v4(), "2026-01-06T10:00:00", l.salary, l.cash_f, "1000.00"),
             operation_row(l.op_kolya, "2026-02-15T10:00:00", l.salary, l.kolya_n, "10.00"),
             operation_row(l.op_give, "2026-03-05T10:00:00", l.card, l.vasya1, "1000.00"),
             operation_row(uuidgen::v4(), "2026-03-06T10:00:00", l.card, l.vasya2, "100.00"),
             operation_row(uuidgen::v4(), "2026-03-07T10:00:00", l.cash_f, l.vasya_f, "50.00"),
             operation_row(l.op_borrow, "2026-03-08T10:00:00", l.petya_e, l.card, "30.00"),
             operation_row(l.op_back, "2026-03-20T10:00:00", l.vasya1, l.card, "400.00"),
             operation_row(l.op_forgive, "2026-03-25T10:00:00", l.vasya2, l.write_off, "20.00"),
             operation_row(l.op_debt2debt, "2026-03-26T10:00:00", l.vasya2, l.petya_e, "5.00"),
             operation_row(uuidgen::v4(), "2026-04-05T10:00:00", l.card, l.vasya1, "999.00"),
         })},
    };
    // Кошельки — kind = 1 (как их шлёт Android).
    for (auto& a : changes["account"]) {
        std::string u = a.at("uuid").get<std::string>();
        if (u != l.salary && u != l.write_off) a["kind"] = 1;
    }
    auto push = sync_repo::push_batch(db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(), changes);
    REQUIRE(push.http_status == 200);
    auto eq = reports_repo::resolve_eq_currency(reports_db, l.iso_e, l.identity.ledger_id);
    REQUIRE(eq.has_value());
    l.eq = *eq;
    return l;
}

const char* kDebtFrom = "2026-03-01";
const char* kDebtTo = "2026-03-31 23:59:59";

std::string str(const json& v) { return v.is_null() ? std::string("null") : v.get<std::string>(); }

} // namespace

TEST(sync_apply_order_matches_wallet_api) {
    // Копия kApplyOrder wallet-api (change debts): counterparty, account_debt
    // — после account_asset, до operation.
    const std::vector<std::string> expected = {
        "icon", "currency", "currency_pair", "currency_pair_rate", "currency_conversion", "account",
        "account_operating", "account_asset", "counterparty", "account_debt", "operation", "operation_link"};
    CHECK(sync_repo::apply_order() == expected);
    // Сид леджера с контрагентами и долгами одним батчем проходит.
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);
    auto res = reports_db->exec(
        "SELECT (SELECT count(*) FROM counterparty WHERE ledger_id = $1), "
        "(SELECT count(*) FROM account_debt WHERE ledger_id = $1 AND deleted_at IS NULL), "
        "(SELECT due_date::text FROM account_debt WHERE ledger_id = $1 AND uuid = $2), "
        "(SELECT coalesce(due_date::text, 'NULL') FROM account_debt WHERE ledger_id = $1 AND uuid = $3)",
        {l.identity.ledger_id, l.vasya1, l.petya_e});
    CHECK_EQ(res.rows.at(0).fields.at(0), std::string("3"));
    CHECK_EQ(res.rows.at(0).fields.at(1), std::string("5"));
    CHECK_EQ(res.rows.at(0).fields.at(2), std::string("2026-12-31"));
    CHECK_EQ(res.rows.at(0).fields.at(3), std::string("NULL"));
}

TEST(balance_separates_debts_from_liquid_total) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);

    auto r = reports_repo::balance_to_date(*reports_db, l.identity.ledger_id, "2026-04-01", l.eq);
    CHECK_EQ(r.total_eq, std::string("6230.00"));     // Карта 4330 + Наличные F 1900
    CHECK(!r.incomplete);                              // Коля без курса — не ликвидный
    CHECK_EQ(r.receivable_eq, std::string("775.00"));  // 600 + 75 + 100
    CHECK_EQ(r.payable_eq, std::string("25.00"));      // Петя −25
    CHECK(r.debts_incomplete);                         // Коля · N без курса
    CHECK_EQ(r.with_debts_eq, std::string("6980.00")); // 6230 + 775 − 25
    CHECK_EQ(r.net_worth_eq, std::string("6230.00"));  // долги в чистый капитал не входят
    CHECK_EQ(r.capital_eq, std::string("0"));

    const json* v1 = find_by(r.rows, "account_uuid", l.vasya1);
    const json* card = find_by(r.rows, "account_uuid", l.card);
    const json* petya = find_by(r.rows, "account_uuid", l.petya_e);
    REQUIRE(v1 && card && petya);
    CHECK(v1->at("counterparty_uuid") == json(l.vasya));
    CHECK(card->at("counterparty_uuid").is_null());
    CHECK_EQ(petya->at("balance").get<std::string>(), std::string("-25.00"));

    // Те же итоги в блоке /balance-at-dates и бакете /balance-series (обе ветки).
    auto at = reports_repo::balances_at_dates(*reports_db, l.identity.ledger_id, {"2026-04-01"}, l.eq);
    REQUIRE(at.dates.size() == static_cast<size_t>(1));
    CHECK_EQ(str(at.dates[0].at("receivable_eq")), std::string("775.00"));
    CHECK_EQ(str(at.dates[0].at("payable_eq")), std::string("25.00"));
    CHECK_EQ(str(at.dates[0].at("with_debts_eq")), std::string("6980.00"));
    CHECK(at.dates[0].at("debts_incomplete").get<bool>());
    const json* at_v1 = find_by(at.dates[0].at("accounts"), "account_uuid", l.vasya1);
    REQUIRE(at_v1 != nullptr);
    CHECK(at_v1->at("counterparty_uuid") == json(l.vasya));
}

TEST(balance_series_debt_totals_match_accounts_breakdown) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);

    auto with = reports_repo::balance_series(*reports_db, l.identity.ledger_id, "2026-01-01", "2026-03-31", "month",
                                             l.eq, true);
    auto without = reports_repo::balance_series(*reports_db, l.identity.ledger_id, "2026-01-01", "2026-03-31", "month",
                                                l.eq, false);
    REQUIRE(with.buckets.size() == static_cast<size_t>(3));
    REQUIRE(without.buckets.size() == with.buckets.size());
    for (std::size_t i = 0; i < with.buckets.size(); ++i) {
        for (const char* k : {"total_eq", "receivable_eq", "payable_eq", "with_debts_eq", "net_worth_eq"}) {
            CHECK_EQ(str(with.buckets[i].at(k)), str(without.buckets[i].at(k)));
        }
        for (const char* k : {"incomplete", "debts_incomplete"}) {
            CHECK(with.buckets[i].at(k) == without.buckets[i].at(k));
        }
        // Итоги долгов = суммы по счетам с контрагентом (курс у всех кроме N).
        double pos = 0, neg = 0;
        for (const auto& a : with.buckets[i].at("accounts")) {
            if (a.at("counterparty_uuid").is_null()) continue;
            std::string eq = a.at("balance_eq").get<std::string>();
            if (eq.empty()) continue;
            double v = std::stod(eq);
            (v > 0 ? pos : neg) += v;
        }
        CHECK(std::fabs(std::stod(str(with.buckets[i].at("receivable_eq"))) - pos) < 0.005);
        CHECK(std::fabs(std::stod(str(with.buckets[i].at("payable_eq"))) + neg) < 0.005);
    }
    // Март: те же числа, что у balance_to_date на 2026-04-01.
    CHECK_EQ(str(without.buckets[2].at("receivable_eq")), std::string("775.00"));
    CHECK_EQ(str(without.buckets[2].at("payable_eq")), std::string("25.00"));
    CHECK(without.buckets[2].at("debts_incomplete").get<bool>());
    CHECK(!without.buckets[2].at("incomplete").get<bool>());
}

TEST(balance_giving_and_taking_loan_keeps_with_debts_total) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);
    // Выдача займа 1000 (2026-03-05) и получение займа 30 (2026-03-08): даты
    // «до» и «после» каждой — без других проводок между ними.
    auto at = reports_repo::balances_at_dates(*reports_db, l.identity.ledger_id,
                                              {"2026-03-05", "2026-03-06", "2026-03-08", "2026-03-09"}, l.eq);
    REQUIRE(at.dates.size() == static_cast<size_t>(4));
    const auto& before_give = at.dates[0];
    const auto& after_give = at.dates[1];
    CHECK_EQ(str(before_give.at("total_eq")), std::string("7000.00"));
    CHECK_EQ(str(after_give.at("total_eq")), std::string("6000.00"));
    CHECK_EQ(str(before_give.at("receivable_eq")), std::string("0"));
    CHECK_EQ(str(after_give.at("receivable_eq")), std::string("1000.00"));
    CHECK_EQ(str(before_give.at("with_debts_eq")), str(after_give.at("with_debts_eq")));

    const auto& before_take = at.dates[2];
    const auto& after_take = at.dates[3];
    CHECK_EQ(str(before_take.at("payable_eq")), std::string("0"));
    CHECK_EQ(str(after_take.at("payable_eq")), std::string("30.00"));
    CHECK_EQ(std::stod(str(after_take.at("total_eq"))) - std::stod(str(before_take.at("total_eq"))), 30.0);
    CHECK_EQ(str(before_take.at("with_debts_eq")), str(after_take.at("with_debts_eq")));
}

TEST(balance_unmarked_debt_returns_to_liquid_total) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);
    json tombstone = debt_row(l.vasya_f, l.vasya, "");
    tombstone["deleted"] = true;
    REQUIRE(sync_repo::push_batch(*db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(),
                                  {{"account_debt", json::array({tombstone})}})
                .http_status == 200);

    auto r = reports_repo::balance_to_date(*reports_db, l.identity.ledger_id, "2026-04-01", l.eq);
    CHECK_EQ(r.total_eq, std::string("6330.00"));    // + Вася F 100
    CHECK_EQ(r.receivable_eq, std::string("675.00"));
    const json* vf = find_by(r.rows, "account_uuid", l.vasya_f);
    REQUIRE(vf != nullptr);
    CHECK(vf->at("counterparty_uuid").is_null());
    CHECK_EQ(vf->at("balance").get<std::string>(), std::string("50.00")); // баланс не тронут
}

TEST(operations_mark_debt_with_counterparty) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);

    auto ops = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, kDebtFrom, kDebtTo, l.eq, 100,
                                                   "", "");
    const json* give = find_by(ops.rows, "operation_uuid", l.op_give);
    const json* borrow = find_by(ops.rows, "operation_uuid", l.op_borrow);
    const json* forgive = find_by(ops.rows, "operation_uuid", l.op_forgive);
    const json* d2d = find_by(ops.rows, "operation_uuid", l.op_debt2debt);
    REQUIRE(give && borrow && forgive && d2d);
    CHECK_EQ(give->at("kind").get<std::string>(), std::string("debt"));
    CHECK_EQ(give->at("to_uuid").get<std::string>(), l.vasya1);
    CHECK(give->at("counterparty_uuid") == json(l.vasya));
    CHECK_EQ(borrow->at("kind").get<std::string>(), std::string("debt"));
    CHECK(borrow->at("counterparty_uuid") == json(l.petya));
    CHECK_EQ(forgive->at("kind").get<std::string>(), std::string("expense"));
    CHECK(forgive->at("counterparty_uuid").is_null());
    CHECK_EQ(d2d->at("kind").get<std::string>(), std::string("transfer"));
    CHECK(d2d->at("counterparty_uuid").is_null());
    // Порядок страницы прежний — date DESC.
    for (std::size_t i = 1; i < ops.rows.size(); ++i) {
        CHECK(ops.rows[i - 1].at("date").get<std::string>() >= ops.rows[i].at("date").get<std::string>());
    }

    reports_repo::OperationsFilter f;
    REQUIRE(!reports_repo::parse_operations_filter("", "", "debt", "", f).has_value());
    auto only = reports_repo::operations_for_period(*reports_db, l.identity.ledger_id, kDebtFrom, kDebtTo, l.eq, 100,
                                                    "", "", f);
    CHECK_EQ(only.rows.size(), static_cast<size_t>(5)); // выдачи Васе ×3, заём у Пети, возврат Васи
    CHECK_EQ(only.total_count, 5LL);
}

TEST(accounts_turnover_mark_debt_and_flows_exclude_loans) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);

    auto a = reports_repo::list_accounts(*reports_db, l.identity.ledger_id, reports_repo::AccountKindFilter::Any, false);
    const json* v1 = find_by(a.accounts, "uuid", l.vasya1);
    const json* card = find_by(a.accounts, "uuid", l.card);
    const json* wo = find_by(a.accounts, "uuid", l.write_off);
    REQUIRE(v1 && card && wo);
    CHECK(v1->at("counterparty_uuid") == json(l.vasya));
    CHECK(card->at("counterparty_uuid").is_null());
    CHECK(wo->at("counterparty_uuid").is_null());

    auto t = reports_repo::wallet_turnover_for_period(*reports_db, l.identity.ledger_id, kDebtFrom, kDebtTo, l.eq);
    const json* tv1 = find_by(t.rows, "account_uuid", l.vasya1);
    REQUIRE(tv1 != nullptr);
    CHECK(tv1->at("counterparty_uuid") == json(l.vasya));
    CHECK_EQ(tv1->at("turnover_in").get<std::string>(), std::string("1000.00"));
    CHECK_EQ(tv1->at("turnover_out").get<std::string>(), std::string("400.00"));

    // За март доходов нет, расход — только прощение 20.
    auto inc = reports_repo::income_for_period(*reports_db, l.identity.ledger_id, kDebtFrom, kDebtTo, l.eq);
    CHECK(inc.rows.empty());
    auto exp = reports_repo::expense_for_period(*reports_db, l.identity.ledger_id, kDebtFrom, kDebtTo, l.eq);
    REQUIRE(exp.rows.size() == static_cast<size_t>(1));
    CHECK_EQ(exp.rows[0].at("account_uuid").get<std::string>(), l.write_off);
    CHECK_EQ(exp.total_eq, std::string("20.00"));
}

TEST(debts_report_groups_by_counterparty_with_totals) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);

    auto d = reports_repo::debts_for_period(*reports_db, l.identity.ledger_id, kDebtFrom, kDebtTo, l.eq);
    REQUIRE(d.counterparties.size() == static_cast<size_t>(3));
    // Порядок — |итог| убыв., без курса в конце.
    const json& vasya = d.counterparties[0];
    const json& petya = d.counterparties[1];
    const json& kolya = d.counterparties[2];
    CHECK_EQ(vasya.at("counterparty_uuid").get<std::string>(), l.vasya);
    CHECK_EQ(petya.at("counterparty_uuid").get<std::string>(), l.petya);
    CHECK_EQ(kolya.at("counterparty_uuid").get<std::string>(), l.kolya);

    // Вася: три кошелька — два займа в E с разными сроками и один в F.
    CHECK_EQ(str(vasya.at("balance_eq")), std::string("775.00"));
    CHECK(!vasya.at("incomplete").get<bool>());
    REQUIRE(vasya.at("accounts").size() == static_cast<size_t>(3));
    const json* v1 = find_by(vasya.at("accounts"), "account_uuid", l.vasya1);
    const json* v2 = find_by(vasya.at("accounts"), "account_uuid", l.vasya2);
    const json* vf = find_by(vasya.at("accounts"), "account_uuid", l.vasya_f);
    REQUIRE(v1 && v2 && vf);
    // Частичный возврат: 0 / 1000 / 400 / 600; 999 после периода не входит.
    CHECK_EQ(str(v1->at("balance_from")), std::string("0.00"));
    CHECK_EQ(str(v1->at("inflow")), std::string("1000.00"));
    CHECK_EQ(str(v1->at("outflow")), std::string("400.00"));
    CHECK_EQ(str(v1->at("balance_to")), std::string("600.00"));
    CHECK_EQ(str(v1->at("due_date")), std::string("2026-12-31"));
    CHECK_EQ(str(v2->at("due_date")), std::string("2027-06-30"));
    CHECK_EQ(str(v2->at("balance_to")), std::string("75.00"));
    CHECK(vf->at("due_date").is_null());
    CHECK_EQ(str(vf->at("balance_to")), std::string("50.00"));
    CHECK_EQ(str(vf->at("balance_to_eq")), std::string("100.00"));

    // Петя: остаток со знаком «я должен».
    CHECK_EQ(str(petya.at("balance_eq")), std::string("-25.00"));
    REQUIRE(petya.at("accounts").size() == static_cast<size_t>(1));
    CHECK_EQ(str(petya.at("accounts")[0].at("balance_to")), std::string("-25.00"));

    // Коля: архивный контрагент, валюта без курса — суммы есть, в итоги не входит.
    CHECK(kolya.at("is_archived").get<bool>());
    CHECK(kolya.at("balance_eq").is_null());
    CHECK(kolya.at("incomplete").get<bool>());
    const json& kn = kolya.at("accounts")[0];
    CHECK_EQ(str(kn.at("balance_from")), std::string("10.00"));
    CHECK_EQ(str(kn.at("balance_to")), std::string("10.00"));
    CHECK(kn.at("balance_to_eq").is_null());

    CHECK_EQ(d.receivable_eq, std::string("775.00"));
    CHECK_EQ(d.payable_eq, std::string("25.00"));
    CHECK_EQ(d.balance_eq, std::string("750.00"));
    CHECK(d.incomplete);
}

TEST(debts_report_overpayment_and_empty_ledger) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);
    // Переплата: Вася 1 вернул ещё 700 — остаток −100, уходит в «я должен».
    REQUIRE(sync_repo::push_batch(*db, l.identity.ledger_id, l.identity.device_id, uuidgen::v4(),
                                  {{"operation", json::array({operation_row(uuidgen::v4(), "2026-03-28T10:00:00",
                                                                            l.vasya1, l.card, "700.00")})}})
                .http_status == 200);
    auto d = reports_repo::debts_for_period(*reports_db, l.identity.ledger_id, kDebtFrom, kDebtTo, l.eq);
    const json* vasya = find_by(d.counterparties, "counterparty_uuid", l.vasya);
    REQUIRE(vasya != nullptr);
    const json* v1 = find_by(vasya->at("accounts"), "account_uuid", l.vasya1);
    REQUIRE(v1 != nullptr);
    CHECK_EQ(str(v1->at("balance_to")), std::string("-100.00"));
    CHECK_EQ(d.receivable_eq, std::string("175.00")); // Вася 2 75 + Вася F 100
    CHECK_EQ(d.payable_eq, std::string("125.00"));    // Вася 1 −100, Петя −25

    // Леджер без долгов — пусто, итоги "0".
    auto other = fixture::make_identity(*db);
    auto e = reports_repo::debts_for_period(*reports_db, other.ledger_id, kDebtFrom, kDebtTo, l.eq);
    CHECK(e.counterparties.empty());
    CHECK_EQ(e.balance_eq, std::string("0"));
    CHECK(!e.incomplete);
}

TEST(counterparties_hide_archived_by_default_and_are_read_only) {
    auto db = fixture::open();
    auto reports_db = fixture::open_reports();
    auto l = seed_debt_ledger(*db, *reports_db);

    auto live = reports_repo::list_counterparties(*reports_db, l.identity.ledger_id, false);
    CHECK_EQ(live.counterparties.size(), static_cast<size_t>(2));
    CHECK(find_by(live.counterparties, "uuid", l.kolya) == nullptr);
    const json* vasya = find_by(live.counterparties, "uuid", l.vasya);
    REQUIRE(vasya != nullptr);
    CHECK(vasya->at("icon_uuid").is_null());
    CHECK(vasya->at("is_archived") == json(false));

    auto all = reports_repo::list_counterparties(*reports_db, l.identity.ledger_id, true);
    CHECK_EQ(all.counterparties.size(), static_cast<size_t>(3));

    // Роль отчётов (wallet_reader) писать в таблицу не может.
    CHECK_THROWS(reports_db->exec("INSERT INTO counterparty (uuid, ledger_id, seq, name) VALUES ($1, $2, 1, 'x')",
                                  {uuidgen::v4(), l.identity.ledger_id}));
}
