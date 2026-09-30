// Разовая утилита нагрузочного контура (wallet-report/loadtest/) —
// НЕ тест: заводит N синтетических пользователей с реалистичным объёмом
// данных (несколько кошельков/валют, набор категорий, тысячи операций за
// ~2 года) через тот же sync_repo::push_batch(), которым в проде пишет
// wallet-api — тот же приём, что и у tests_integration/test_reports.cpp (см.
// её README.md, "Почему push_batch, а не SQL"): отчёты должны увидеть данные
// ровно так же, как их видит протокол синка, а не какой-то отдельный способ
// записи в обход него.
//
// Пишет НЕ в одноразовую тестовую базу (как run_local.sh для интеграционных
// тестов), а в ту, что укажет DATABASE_URL — обычно это dev-база уже
// поднятого `podman compose` стека, чтобы нагрузка потом била ровно в тот
// контейнер wallet-report, который тюнят (см. loadtest/README.md).
//
// Результат — JSON-файл (LOADTEST_USERS_JSON) со списком заведённых
// пользователей: вход для mint_tokens.sh (следующий шаг контура).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "db.hpp"
#include "fixture.hpp"
#include "nlohmann/json.hpp"
#include "provisioning.hpp"
#include "sync_repo.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

int env_int(const char* key, int fallback) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return fallback;
    try {
        return std::stoi(v);
    } catch (const std::exception&) {
        return fallback;
    }
}

// Те же билдеры JSON-строк, что в test_reports.cpp — небольшой
// самостоятельный дубль (не общий заголовок), тем же приёмом, что и весь
// остальной проект: каждый .cpp здесь самодостаточен.
json currency_row(const std::string& uuid, const std::string& icon_uuid, const std::string& iso_code,
                   const std::string& name, const std::string& symbol) {
    return json{{"uuid", uuid},       {"name", name},          {"iso_code", iso_code},
                {"icon_uuid", icon_uuid}, {"symbol", symbol}, {"precision", 2}};
}

json account_row(const std::string& uuid, const std::string& icon_uuid, const std::string& name) {
    return json{{"uuid", uuid}, {"name", name}, {"comment", ""}, {"icon_uuid", icon_uuid},
                {"is_archived", false}, {"system_kind", 0}, {"kind", 0}};
}

json account_operating_row(const std::string& account_uuid, const std::string& currency_uuid) {
    return json{{"uuid", account_uuid}, {"currency_uuid", currency_uuid}};
}

json operation_row(const std::string& uuid, const std::string& date, const std::string& from_uuid,
                    const std::string& to_uuid, const std::string& amount) {
    return json{{"uuid", uuid},          {"date", date},      {"acc_fr_uuid", from_uuid},
                {"acc_to_uuid", to_uuid}, {"amount", amount}, {"comment", "loadtest seed"}};
}

std::string format_date(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

struct SeededUser {
    std::string user_id;
    std::string email;
    std::string ledger_id;
};

void push_or_throw(db::Database& db, const std::string& ledger_id, const std::string& device_id,
                    const std::string& email, const json& changes) {
    auto push = sync_repo::push_batch(db, ledger_id, device_id, uuidgen::v4(), changes);
    if (push.http_status != 200) {
        throw std::runtime_error("push_batch для " + email + " вернул " +
                                  std::to_string(push.http_status) + ": " + push.response_body);
    }
}

// Заводит одного синтетического пользователя: три кошелька (два в USD, один
// в EUR) + пять категорий, `ops_count` операций/переводов вперемешку,
// разбросанных по `days_span` дням назад от сегодня — чтобы тяжёлые отчёты
// (balance-series, пересчитывающий бакеты с начала истории) реально
// нагружали Postgres, а не отвечали мгновенно на пустых данных.
SeededUser seed_one_user(db::Database& db, int index, const std::string& icon_uuid, int ops_count,
                          int days_span, std::mt19937& rng) {
    std::string user_id = uuidgen::v4();
    std::string device_id = uuidgen::v4();
    std::string email = "loadtest-" + std::to_string(index) + "@loadtest.example.com";

    auto identity =
        provisioning::ensure_identity(db, user_id, email, device_id, "loadtest-device", "other");

    // iso_code уникален только В ПРЕДЕЛАХ леджера (uq_currency_iso_ledger,
    // см. wallet-api/schema/table/currency.sql) — одни и те же коды у каждого
    // синтетического пользователя безопасны, push_batch сам подставляет
    // АУТЕНТИФИЦИРОВАННЫЙ ledger_id, не тот, что в теле запроса.
    std::string usd = uuidgen::v4();
    std::string eur = uuidgen::v4();

    std::string wallet_usd_1 = uuidgen::v4();
    std::string wallet_usd_2 = uuidgen::v4();
    std::string wallet_eur = uuidgen::v4();
    std::string cat_groceries = uuidgen::v4();
    std::string cat_salary = uuidgen::v4();
    std::string cat_transport = uuidgen::v4();
    std::string cat_entertainment = uuidgen::v4();
    std::string cat_utilities = uuidgen::v4();

    json setup = {
        {"currency", json::array({currency_row(usd, icon_uuid, "USD", "US Dollar", "$"),
                                   currency_row(eur, icon_uuid, "EUR", "Euro", "\xE2\x82\xAC")})},
        {"account", json::array({
                        account_row(wallet_usd_1, icon_uuid, "Main wallet"),
                        account_row(wallet_usd_2, icon_uuid, "Savings"),
                        account_row(wallet_eur, icon_uuid, "Euro wallet"),
                        account_row(cat_groceries, icon_uuid, "Groceries"),
                        account_row(cat_salary, icon_uuid, "Salary"),
                        account_row(cat_transport, icon_uuid, "Transport"),
                        account_row(cat_entertainment, icon_uuid, "Entertainment"),
                        account_row(cat_utilities, icon_uuid, "Utilities"),
                    })},
        {"account_operating", json::array({
                                  account_operating_row(wallet_usd_1, usd),
                                  account_operating_row(wallet_usd_2, usd),
                                  account_operating_row(wallet_eur, eur),
                              })},
    };
    push_or_throw(db, identity.ledger_id, device_id, email, setup);

    std::vector<std::string> wallets = {wallet_usd_1, wallet_usd_2, wallet_eur};
    std::vector<std::string> categories = {cat_groceries, cat_salary, cat_transport, cat_entertainment,
                                            cat_utilities};

    std::uniform_int_distribution<int> day_offset(0, days_span);
    std::uniform_int_distribution<int> hour_offset(8, 21);
    std::uniform_int_distribution<std::size_t> wallet_pick(0, wallets.size() - 1);
    std::uniform_int_distribution<std::size_t> category_pick(0, categories.size() - 1);
    std::uniform_real_distribution<double> amount_pick(5.0, 500.0);
    std::uniform_int_distribution<int> kind_pick(0, 9); // 0 -> перевод кошелёк-кошелёк (10%)

    auto now = std::chrono::system_clock::now();

    // 500 операций в батче — компромисс между числом push_batch-вызовов
    // (каждый — отдельная транзакция) и размером одной транзакции; порядок
    // величины, а не точный расчёт.
    constexpr int kBatchSize = 500;
    json ops = json::array();
    auto flush = [&]() {
        if (ops.empty()) return;
        push_or_throw(db, identity.ledger_id, device_id, email, json{{"operation", ops}});
        ops = json::array();
    };

    for (int i = 0; i < ops_count; ++i) {
        auto tp = now - std::chrono::hours(24 * day_offset(rng)) - std::chrono::hours(hour_offset(rng));
        std::string date = format_date(tp);
        char amount[16];
        std::snprintf(amount, sizeof(amount), "%.2f", amount_pick(rng));

        std::string from_uuid;
        std::string to_uuid;
        if (kind_pick(rng) == 0) {
            // Перевод: кошелёк -> кошелёк, гарантированно разные (см.
            // operations_for_period_classifies_kind... в test_reports.cpp —
            // ровно этот паттерн сервер видит как kind="transfer").
            std::size_t a = wallet_pick(rng);
            std::size_t b = wallet_pick(rng);
            if (a == b) b = (b + 1) % wallets.size();
            from_uuid = wallets[a];
            to_uuid = wallets[b];
        } else if (i % 2 == 0) {
            from_uuid = wallets[wallet_pick(rng)];   // расход: кошелёк -> категория
            to_uuid = categories[category_pick(rng)];
        } else {
            from_uuid = categories[category_pick(rng)]; // доход: категория -> кошелёк
            to_uuid = wallets[wallet_pick(rng)];
        }

        ops.push_back(operation_row(uuidgen::v4(), date, from_uuid, to_uuid, amount));
        if (static_cast<int>(ops.size()) >= kBatchSize) flush();
    }
    flush();

    return {user_id, email, identity.ledger_id};
}

} // namespace

int main() {
    std::string database_url = env_or("DATABASE_URL", "");
    if (database_url.empty()) {
        std::cerr << "DATABASE_URL не задан — должен указывать на dev-базу стека "
                     "(роль с правом записи, обычно WALLET_POSTGRES_USER, порт 15432 — "
                     "см. wallet-report/loadtest/README.md), НЕ на wallet_reader "
                     "(та роль read-only, писать не может)."
                  << std::endl;
        return 1;
    }

    int users_count = env_int("LOADTEST_USERS", 5);
    int ops_per_user = env_int("LOADTEST_OPS_PER_USER", 3000);
    int days_span = env_int("LOADTEST_DAYS_SPAN", 730);
    std::string out_path = env_or("LOADTEST_USERS_JSON", "loadtest/data/users.json");

    try {
        db::Database db(database_url);

        // Общая (ledger_id IS NULL) иконка — переиспользуется всеми
        // пользователями и обеими валютами каждого, ровно как задумано
        // fixture::make_reference_icon (не привязана к конкретному леджеру).
        std::string icon_uuid = fixture::make_reference_icon(db);

        std::mt19937 rng(42); // фиксированный seed — прогон воспроизводим

        json out = json::array();
        for (int i = 0; i < users_count; ++i) {
            std::cout << "Сею пользователя " << (i + 1) << "/" << users_count << "..." << std::endl;
            SeededUser user = seed_one_user(db, i, icon_uuid, ops_per_user, days_span, rng);
            out.push_back(
                {{"user_id", user.user_id}, {"email", user.email}, {"ledger_id", user.ledger_id}});
        }

        std::ofstream f(out_path);
        if (!f) {
            std::cerr << "Не удалось открыть " << out_path << " на запись" << std::endl;
            return 1;
        }
        f << out.dump(2);
        std::cout << "Готово: " << users_count << " пользователей x " << ops_per_user
                  << " операций, файл " << out_path << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
