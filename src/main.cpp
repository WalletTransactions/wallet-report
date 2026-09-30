// wallet-report — сервис отчётов, вынесенный из wallet-api (CQRS-lite: та же
// шареная Postgres-база домена ledger/sync, что у wallet-api, но отдельный
// процесс с ЕДИНСТВЕННЫМ соединением ролью wallet_reader — см. README,
// "Шареная БД, разные роли"). Сам не логинит и не выпускает токены (этим
// занимается auth-сервис, google-auth) — только проверяет уже выданный
// access-токен по JWKS, тем же приёмом, что wallet-api/wallet-gateway.
//
// Никогда не провизионирует леджер и не пишет ни одной строки — роль, которой
// подключается процесс, физически не имеет прав INSERT/UPDATE/DELETE (см.
// wallet-api/schema/role/wallet_reader.sql), поэтому даже гипотетический баг в
// отчётном коде не смог бы ничего испортить.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>

#include "access.hpp"
#include "app_state.hpp"
#include "docs.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "reports.hpp"
#include "util/dotenv.hpp"
#include "util/jwks.hpp"
#include "util/log.hpp"
#include "util/metrics.hpp"
#include "version.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

#ifdef __GLIBC__
// Порог "крупного" ответа для malloc_trim в логгере (см. set_logger ниже).
constexpr std::size_t kTrimAfterResponseBytes = 256 * 1024;
#endif

#ifdef _WIN32
void fix_console_encoding() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
void fix_console_encoding() {}
#endif

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

std::string env_required(const char* key, const std::string& hint) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') {
        applog::error(std::string(key) + " must be set — " + hint + " (см. README.md)");
        std::exit(1);
    }
    return std::string(v);
}

bool env_bool(const char* key, bool fallback) {
    const char* v = std::getenv(key);
    if (v == nullptr) return fallback;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "True";
}

// "a, b" -> {"a", "b"} — тот же приём, что и в остальных сервисах проекта
// (см. wallet-api/src/main.cpp, parse_csv_list), для LOG_SQL_MASK_PARAMS.
std::unordered_set<std::string> parse_csv_list(const std::string& csv) {
    std::unordered_set<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), not_space));
        item.erase(std::find_if(item.rbegin(), item.rend(), not_space).base(), item.end());
        if (!item.empty()) out.insert(item);
    }
    return out;
}

std::string strip_trailing_slash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

// Не льём в лог мегабайтные тела целиком — отчёты (особенно /operations) тоже
// могут быть немаленькими.
//
// Лимит — в СЫРЫХ байтах тела, не в байтах итоговой строки лога: на
// бинарных телах непечатные байты при JSON-экранировании раздуваются до
// 6 символов каждый — при старом лимите 4096 байт это ломало journald
// (резал строку на куски, `CONTAINER_PARTIAL_MESSAGE:true`) и fluent-bit
// (строгий JSON-парсер app_json падал на оборванном куске, запись
// оставалась без level/service/trace_id/msg — только сырой "message"),
// см. подробный разбор в wallet-api/src/main.cpp.
constexpr std::size_t kMaxLoggedBodyBytes = 1024;

std::string truncate_body(const std::string& body) {
    if (body.size() <= kMaxLoggedBodyBytes) return body;
    return body.substr(0, kMaxLoggedBodyBytes) + "...(обрезано, всего " +
           std::to_string(body.size()) + " байт)";
}

// Тело ответа для debug-лога. httplib сжимает res.body ДО вызова логгера
// (CPPHTTPLIB_ZLIB_SUPPORT + Accept-Encoding клиента, см. CMakeLists.txt) —
// в лог шли байты gzip, нечитаемые. Сжатое тело не пишем, только кодировку
// и размер.
std::string loggable_response_body(const httplib::Response& res) {
    std::string encoding = res.get_header_value("Content-Encoding");
    if (!encoding.empty()) return "(сжато " + encoding + ", " + std::to_string(res.body.size()) + " байт)";
    return truncate_body(res.body);
}

std::string default_log_dir() {
#ifdef _WIN32
    return "logs";
#else
    return "";
#endif
}

// Читает trace_id, который поставил wallet-gateway (единственная точка
// входа, см. его main.cpp, ensure_trace_id) — сам НИКОГДА не порождает
// новый, тем же приёмом, что и wallet-api (см. его main.cpp).
httplib::Server::HandlerResponse handle_pre_routing(const httplib::Request& req,
                                                     httplib::Response& res) {
    applog::start_request_timer();
    std::string trace_id = req.get_header_value("X-Trace-Id");
    if (!trace_id.empty()) {
        applog::set_trace_id(trace_id);
        res.set_header("X-Trace-Id", trace_id);
    }
    return httplib::Server::HandlerResponse::Unhandled;
}

} // namespace

int main() {
    fix_console_encoding();
    dotenv::load(".env");

    std::string log_dir = env_or("LOG_DIR", default_log_dir());
    if (log_dir == "stdout" || log_dir == "-") log_dir.clear();
    applog::init(log_dir, "wallet-report");

    // build_info для /metrics (см. util/metrics.cpp) — один раз здесь, до
    // svr.listen() (см. GET /version, docs.cpp — та же тройка полей).
    metrics::set_build_info(version::kService, version::kVersion, version::kGitCommit);

    AppState state;

    std::string auth_service_url = strip_trailing_slash(
        env_required("AUTH_SERVICE_URL", "адрес auth-сервиса, например http://127.0.0.1:8080"));
    state.jwks = std::make_unique<jwks::Cache>(auth_service_url);

    // Пул соединений с БД — в отличие от wallet-api тут нет DATABASE_URL
    // вовсе: см. app_state.hpp и README, "Read-only по конструкции". Ролью
    // wallet_reader к ТОЙ ЖЕ базе, что у wallet-api. REPORTS_DB_POOL_SIZE —
    // сколько соединений держать одновременно (см. db.hpp::Pool, зачем
    // вообще пул, а не одно, как раньше) — 8 по умолчанию: с запасом
    // покрывает пачку параллельных GET-запросов одного открытия "Отчётов" на
    // клиенте (см. её комментарий), не открывая существенно больше
    // соединений к Postgres, чем нужно одному активно работающему процессу.
    bool log_sql_params = env_bool("LOG_SQL_PARAMS", false);
    auto log_sql_mask_params = parse_csv_list(env_or("LOG_SQL_MASK_PARAMS", ""));
    std::string reports_database_url = env_required(
        "REPORTS_DATABASE_URL",
        "строка подключения ролью wallet_reader к базе wallet-api, например "
        "postgresql://wallet_reader:wallet_reader@localhost:5432/wallet_api");
    int reports_db_pool_size = std::stoi(env_or("REPORTS_DB_POOL_SIZE", "8"));
    // < 1 отдельно от Pool(size == 0) — там size типа std::size_t, и
    // отрицательное int при касте в него стало бы огромным числом (попыткой
    // открыть миллиарды соединений), а не понятной ошибкой "size == 0".
    if (reports_db_pool_size < 1) {
        applog::error("REPORTS_DB_POOL_SIZE должен быть положительным целым, получено: " +
                      std::to_string(reports_db_pool_size));
        return 1;
    }
    try {
        state.reports_pool = std::make_unique<db::Pool>(reports_database_url,
                                                          static_cast<std::size_t>(reports_db_pool_size),
                                                          log_sql_params, log_sql_mask_params);
    } catch (const std::exception& ex) {
        applog::error(std::string("Postgres (reports_pool): ") + ex.what());
        return 1;
    }

    std::string host = env_or("HOST", "127.0.0.1");
    int port = std::stoi(env_or("PORT", "8084"));

    httplib::Server svr;
    // Трейс-контекст, затем iss/aud/scope токена (access.hpp) — до
    // хендлеров, одной точкой на все маршруты.
    svr.set_pre_routing_handler([&state](const httplib::Request& req, httplib::Response& res) {
        handle_pre_routing(req, res);
        return authz::gate(req, res, *state.jwks);
    });
    reportsapi::register_routes(svr, state);
    docsapi::register_routes(svr); // GET /openapi.yaml, /docs, /healthz, статика /vendor/swagger-ui/*

    // Та же последняя линия обороны, что и в wallet-api — см. его main.cpp,
    // почему это не замена try/catch по месту.
    svr.set_exception_handler(
        [](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
            std::string message = "internal error";
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                message = e.what();
            }
            applog::error("unhandled exception: " + req.method + " " + req.path + " -> " + message);
            res.status = 500;
            res.set_content(nlohmann::json{{"error", "internal server error"}}.dump(),
                             "application/json");
        });

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        // Метрики — на ВСЕ запросы, включая /healthz и сам /metrics: в
        // отличие от access-лога ниже, кардинальность здесь уже ограничена
        // applog::current_route() (см. util/metrics.hpp), а знать RPS/
        // латентность healthcheck'а тоже полезно (отличить "не отвечает
        // вовсе" от "отвечает медленно"). route выставляется каждым
        // хендлером через applog::set_route() — если хендлер (докстраница,
        // /healthz, /metrics) этого не делает, current_route() вернётся
        // пустой, "(unset)" группирует такие запросы одним лейблом.
        metrics::observe_http_request(
            req.method, applog::current_route().empty() ? "(unset)" : applog::current_route(),
            res.status, applog::request_duration_ms());

        // /healthz — не запрос приложения, а probe compose'а (interval 10s,
        // см. compose.yaml) — по логам он не отличим от реального трафика,
        // но забивает их шумом (~8640 строк/сутки) без всякой пользы: сама
        // проверка ничего не открывает и не пишет. clear_request_context()
        // всё равно нужен (thread_local не должен утечь в следующий, никак
        // не связанный запрос на этом потоке) — выходим, не пропуская его,
        // а не просто пропускаем обе строки лога.
        // /metrics — то же самое: скрейп Prometheus каждые 15 с, в теле —
        // текст метрик (или gzip), для разбора инцидентов бесполезный.
        if (req.path == "/healthz" || req.path == "/metrics") {
            applog::clear_request_context();
            return;
        }
        applog::info(req.method + " " + req.path + " -> " + std::to_string(res.status),
                     {{"event", "request_finished"}, {"duration_ms", applog::request_duration_ms()}});
        applog::debug(req.method + " " + req.path,
                      {{"event", "request_body"},
                       {"request_body", truncate_body(req.body)},
                       {"response_body", loggable_response_body(res)}});
        applog::clear_request_context();

#ifdef __GLIBC__
        // Крупный ответ (по прямому запросу, 2026-09-22 — "надуло ОЗУ до
        // 300 МБ и память не убывает") — к этому моменту дерево json и
        // PGresult уже освобождены, но glibc держит освободившиеся страницы в
        // арене потока, RSS не падает никогда. malloc_trim отдаёт их ОС; зовём
        // только после больших ответов — на мелких это лишняя работа на
        // каждый запрос. Число арен ограничено MALLOC_ARENA_MAX (compose.yaml).
        if (res.body.size() >= kTrimAfterResponseBytes) malloc_trim(0);
#endif
    });

    applog::info("wallet-report v" + std::string(version::kVersion) + " (" +
                 std::string(version::kGitCommit) + ") listening on http://" + host + ":" +
                 std::to_string(port) + " (auth-сервис: " + auth_service_url + ")" +
                 (log_dir.empty() ? "" : " (логи: " + log_dir + "/)"));
    if (!svr.listen(host, port)) {
        applog::error("не удалось запустить сервер на " + host + ":" + std::to_string(port) +
                      " (порт занят или недоступен?)");
        return 1;
    }
    return 0;
}
