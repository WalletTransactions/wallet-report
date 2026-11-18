// Эквивалент по маршрутам пересчёта валют (change currency-conversion-routes,
// спека balance-reports) против живого Postgres, ролью wallet_reader: отчёты
// зовут SQL-функции wallet-api (f_balance_to_dates, f_balance_series), а те берут
// курс из rate_on — прямой курс → явный маршрут → кросс через USD. Здесь
// проверяется результат на уровне отчёта: сумма эквивалента, признак incomplete и
// отсутствие дублей строк.
//
// Данные сеются через sync_repo::push_batch (как в test_reports.cpp). Валюты —
// свои у леджера теста; «USD» хаба — собственная валюта с iso_code = 'USD' (своя
// перекрывает общую), остальные коды — случайные.
#include "reports_repo.hpp"

#include <string>
#include <vector>

#include "fixture.hpp"
#include "nlohmann/json.hpp"
#include "sync_repo.hpp"
#include "test_framework.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace {

json currency_row(const std::string& uuid, const std::string& icon_uuid, const std::string& iso_code) {
    return json{{"uuid", uuid},        {"name", iso_code + " " + uuid}, {"iso_code", iso_code},
                {"icon_uuid", icon_uuid}, {"symbol", iso_code},          {"precision", 2}};
}

json account_row(const std::string& uuid, const std::string& icon_uuid, const std::string& name) {
    return json{{"uuid", uuid}, {"name", name}, {"comment", ""}, {"icon_uuid", icon_uuid},
                {"is_archived", false}, {"system_kind", 0}, {"kind", 0}};
}

json pair_row(const std::string& uuid, const std::string& symbol, const std::string& base,
              const std::string& quote) {
    return json{{"uuid", uuid}, {"symbol", symbol}, {"base_currency_uuid", base},
                {"quote_currency_uuid", quote}, {"precision", 4}};
}

json rate_row(const std::string& pair_uuid, const std::string& date, const std::string& rate) {
    return json{{"uuid", uuidgen::v4()}, {"currency_pair_uuid", pair_uuid}, {"date", date}, {"rate", rate}};
}

json step_row(const std::string& from, const std::string& to, int num, const std::string& pair,
              bool is_reverse = false) {
    return json{{"uuid", uuidgen::v4()}, {"from_currency_uuid", from}, {"to_currency_uuid", to},
                {"num", num}, {"currency_pair_uuid", pair}, {"is_reverse", is_reverse}};
}

// Леджер с валютами BTC, EUR, RUB, USD (RUB — валюта эквивалента) и кошельком в
// BTC на 2 BTC (поступление 2026-03-05). Пары, курсы и шаги добавляет тест.
struct Market {
    std::shared_ptr<db::Database> db;
    std::shared_ptr<db::Database> reports_db;
    fixture::TestIdentity identity;
    std::string icon;
    std::string btc, eur, rub, usd;
    std::string iso_rub;
    std::string wallet;
};

Market make_market() {
    Market m;
    m.db = fixture::open();
    m.reports_db = fixture::open_reports();
    m.identity = fixture::make_identity(*m.db);
    m.icon = fixture::make_reference_icon(*m.db);
    m.btc = uuidgen::v4();
    m.eur = uuidgen::v4();
    m.rub = uuidgen::v4();
    m.usd = uuidgen::v4();
    m.iso_rub = "RB" + uuidgen::v4().substr(0, 2);
    std::string category = uuidgen::v4();
    m.wallet = uuidgen::v4();
    json changes = {
        {"currency", json::array({currency_row(m.btc, m.icon, "BT" + uuidgen::v4().substr(0, 2)),
                                  currency_row(m.eur, m.icon, "EU" + uuidgen::v4().substr(0, 2)),
                                  currency_row(m.rub, m.icon, m.iso_rub), currency_row(m.usd, m.icon, "USD")})},
        {"account", json::array({account_row(category, m.icon, "Cat " + category),
                                  account_row(m.wallet, m.icon, "BTC wallet " + m.wallet)})},
        {"account_operating", json::array({json{{"uuid", m.wallet}, {"currency_uuid", m.btc}}})},
        {"operation", json::array({json{{"uuid", uuidgen::v4()}, {"date", "2026-03-05T10:00:00"},
                                        {"acc_fr_uuid", category}, {"acc_to_uuid", m.wallet},
                                        {"amount", "2.00"}, {"comment", "routes test"}}})},
    };
    auto out = sync_repo::push_batch(*m.db, m.identity.ledger_id, m.identity.device_id, uuidgen::v4(), changes);
    if (out.http_status != 200) throw std::runtime_error("market seed failed: " + out.response_body);
    return m;
}

void push(Market& m, const json& changes) {
    auto out = sync_repo::push_batch(*m.db, m.identity.ledger_id, m.identity.device_id, uuidgen::v4(), changes);
    REQUIRE(out.http_status == 200);
}

// Пара с курсом на 2026-01-01 и её uuid.
std::string pair_with_rate(Market& m, const std::string& symbol, const std::string& base, const std::string& quote,
                           const std::string& rate) {
    std::string pair = uuidgen::v4();
    push(m, {{"currency_pair", json::array({pair_row(pair, symbol, base, quote)})},
             {"currency_pair_rate", json::array({rate_row(pair, "2026-01-01", rate)})}});
    return pair;
}

// Пара без единого курса.
std::string pair_without_rate(Market& m, const std::string& symbol, const std::string& base, const std::string& quote) {
    std::string pair = uuidgen::v4();
    push(m, {{"currency_pair", json::array({pair_row(pair, symbol, base, quote)})}});
    return pair;
}

void add_route(Market& m, const std::vector<std::pair<std::string, bool>>& pairs, const std::string& from,
               const std::string& to) {
    json steps = json::array();
    int num = 1;
    for (const auto& [pair, reversed] : pairs) steps.push_back(step_row(from, to, num++, pair, reversed));
    push(m, {{"currency_conversion", steps}});
}

// Блок остатков на 2026-04-01: (balance_eq кошелька, incomplete блока).
std::pair<std::string, bool> equivalent_on_april(Market& m) {
    auto eq = reports_repo::resolve_eq_currency(*m.reports_db, m.iso_rub, m.identity.ledger_id);
    REQUIRE(eq.has_value());
    auto result = reports_repo::balances_at_dates(*m.reports_db, m.identity.ledger_id, {"2026-04-01T00:00:00"}, *eq);
    REQUIRE(result.dates.size() == static_cast<size_t>(1));
    const auto& block = result.dates[0];
    REQUIRE(block.at("accounts").size() == static_cast<size_t>(1));
    return {block.at("accounts")[0].at("balance_eq").get<std::string>(), block.at("incomplete").get<bool>()};
}

} // namespace

TEST(routes_report_equivalent_goes_through_explicit_route) {
    auto m = make_market();
    std::string p1 = pair_with_rate(m, "BTCEUR", m.btc, m.eur, "90000");
    std::string p2 = pair_with_rate(m, "EURRUB", m.eur, m.rub, "100");
    add_route(m, {{p1, false}, {p2, false}}, m.btc, m.rub);
    auto [eq, incomplete] = equivalent_on_april(m);
    CHECK_EQ(eq, std::string("18000000.00")); // 2 BTC × 90 000 × 100
    CHECK(!incomplete);
}

TEST(routes_report_equivalent_goes_through_usd_cross_by_default) {
    auto m = make_market();
    pair_with_rate(m, "BTCUSD", m.btc, m.usd, "60000");
    pair_with_rate(m, "USDRUB", m.usd, m.rub, "90");
    auto [eq, incomplete] = equivalent_on_april(m);
    CHECK_EQ(eq, std::string("10800000.00")); // 2 BTC × 60 000 × 90
    CHECK(!incomplete);
}

TEST(routes_report_direct_pair_beats_route_and_usd) {
    auto m = make_market();
    std::string p1 = pair_with_rate(m, "BTCEUR", m.btc, m.eur, "90000");
    std::string p2 = pair_with_rate(m, "EURRUB", m.eur, m.rub, "100");
    add_route(m, {{p1, false}, {p2, false}}, m.btc, m.rub);
    pair_with_rate(m, "BTCUSD", m.btc, m.usd, "60000");
    pair_with_rate(m, "USDRUB", m.usd, m.rub, "90");
    pair_with_rate(m, "BTCRUB", m.btc, m.rub, "5000000");
    auto [eq, incomplete] = equivalent_on_april(m);
    CHECK_EQ(eq, std::string("10000000.00")); // 2 BTC × 5 000 000 — прямая пара
    CHECK(!incomplete);
}

TEST(routes_report_route_wins_over_usd) {
    auto m = make_market();
    std::string p1 = pair_with_rate(m, "BTCEUR", m.btc, m.eur, "90000");
    std::string p2 = pair_with_rate(m, "EURRUB", m.eur, m.rub, "100");
    add_route(m, {{p1, false}, {p2, false}}, m.btc, m.rub);
    pair_with_rate(m, "BTCUSD", m.btc, m.usd, "60000");
    pair_with_rate(m, "USDRUB", m.usd, m.rub, "90");
    auto [eq, incomplete] = equivalent_on_april(m);
    CHECK_EQ(eq, std::string("18000000.00")); // через EUR, а не через USD
    CHECK(!incomplete);
}

TEST(routes_report_broken_route_gives_no_equivalent_and_blocks_usd) {
    auto m = make_market();
    std::string p1 = pair_with_rate(m, "BTCEUR", m.btc, m.eur, "90000");
    std::string p2 = pair_without_rate(m, "EURRUB", m.eur, m.rub); // у шага 2 курсов нет
    add_route(m, {{p1, false}, {p2, false}}, m.btc, m.rub);
    pair_with_rate(m, "BTCUSD", m.btc, m.usd, "60000");
    pair_with_rate(m, "USDRUB", m.usd, m.rub, "90");
    auto [eq, incomplete] = equivalent_on_april(m);
    CHECK_EQ(eq, std::string("")); // пустой эквивалент, USD не подставлен
    CHECK(incomplete);
}

TEST(routes_report_no_pair_no_route_no_usd_gives_no_equivalent) {
    auto m = make_market();
    pair_with_rate(m, "BTCEUR", m.btc, m.eur, "90000"); // есть BTC→EUR, но до RUB пути нет
    auto [eq, incomplete] = equivalent_on_april(m);
    CHECK_EQ(eq, std::string(""));
    CHECK(incomplete);
}

TEST(routes_report_series_has_one_row_per_wallet_and_bucket) {
    auto m = make_market();
    std::string p1 = pair_with_rate(m, "BTCEUR", m.btc, m.eur, "90000");
    std::string p2 = pair_with_rate(m, "EURRUB", m.eur, m.rub, "100");
    add_route(m, {{p1, false}, {p2, false}}, m.btc, m.rub);
    // Курс шага 1 меняется в середине периода: на каждый бакет — своё значение, без дублей строк кошелька.
    push(m, {{"currency_pair_rate", json::array({rate_row(p1, "2026-05-10", "100000")})}});

    auto eq = reports_repo::resolve_eq_currency(*m.reports_db, m.iso_rub, m.identity.ledger_id);
    REQUIRE(eq.has_value());
    auto result = reports_repo::balance_series(*m.reports_db, m.identity.ledger_id, "2026-04-01", "2026-06-30",
                                                "month", *eq, true);
    REQUIRE(result.buckets.size() == static_cast<size_t>(3));
    for (const auto& bucket : result.buckets) {
        REQUIRE(bucket.at("accounts").size() == static_cast<size_t>(1)); // ровно одна строка кошелька
        CHECK(!bucket.at("incomplete").get<bool>());
    }
    // Бакет «день после конца»: апрель закрывается 05-01 (курс 90 000), май — 06-01 (уже 100 000).
    CHECK_EQ(result.buckets[0].at("accounts")[0].at("balance_eq").get<std::string>(), std::string("18000000.00"));
    CHECK_EQ(result.buckets[1].at("accounts")[0].at("balance_eq").get<std::string>(), std::string("20000000.00"));
}
