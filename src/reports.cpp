#include "reports.hpp"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "reports_repo.hpp"
#include "session.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace reportsapi {

namespace {

constexpr const char* kJsonContentType = "application/json";

void json_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content(json{{"error", message}}.dump(), kJsonContentType);
}

// "Посмотреть как клиент" (по прямому запросу — админ выбирает пользователя
// в /admin/users и открывает те же экраны "Отчётов", что видит сам клиент).
// Вместо отдельных /admin/users/:id/reports/* маршрутов (и правки
// маршрутизации wallet-gateway под них) — те же самые /api/reports/*,
// просто с необязательным query-параметром as_user_id: обычный запрос его
// не передаёт вовсе и получает СВОЙ леджер, как и раньше; админ передаёт
// as_user_id — и получает леджер ЭТОГО пользователя, тем же кодом отчёта,
// той же СХЕМОЙ ответа, никакой отдельной "админской" копии каждого
// отчёта.
//
// Роль проверяется здесь, а не молча игнорируется у не-админа: параметр,
// который тихо не сработал бы у обычного пользователя, было бы легко не
// заметить (кнопка "посмотреть как" по ошибке показалась не тому) — явный
// 403 делает такую ошибку видимой сразу, а не как "почему-то показывает
// мои же данные".
std::optional<std::string> resolve_subject_user_id(const session::User& user,
                                                     const httplib::Request& req,
                                                     httplib::Response& res) {
    if (!req.has_param("as_user_id") || req.get_param_value("as_user_id").empty()) {
        return user.user_id;
    }
    if (user.role != "admin") {
        json_error(res, 403, "as_user_id: доступно только роли admin");
        return std::nullopt;
    }
    std::string target_user_id = req.get_param_value("as_user_id");
    // Аудит: кто из админов и чей леджер смотрел (см. README, "Посмотреть
    // как клиент") — единственное место, где это решается, поэтому лог
    // пишется именно тут, не в каждом отдельном handle_*.
    applog::info("admin view-as reports", {{"event", "admin_view_as"},
                                            {"admin_user_id", user.user_id},
                                            {"target_user_id", target_user_id}});
    return target_user_id;
}

// Общий пролог всех трёх отчётов: аутентификация + резолв леджера + резолв
// eq_currency. nullopt — хендлер уже ответил сам (401/400), вызывающий код
// должен просто вернуться. Пустая (не nullopt) ledger_id — легитимный
// случай "леджера ещё нет", отчёт вызывающий код обязан обработать сам (см.
// handle_balance и т.д.), не через этот пролог: разным отчётам нужны разные
// наборы query-параметров (date vs from+to), поэтому только общая часть
// вынесена сюда.
struct Context {
    std::string ledger_id; // пусто, если у пользователя ещё нет ни одного леджера
    std::string eq_currency_uuid;
};

// Пролог для /currencies — тоньше common_prolog: только аутентификация +
// резолв леджера, БЕЗ eq_currency. Список валют существует именно ДЛЯ ТОГО,
// чтобы им заполнить сам выбор эквивалента — требовать eq_currency, чтобы
// узнать, из чего его вообще выбирать, было бы циклической зависимостью.
// nullopt — хендлер уже ответил 401 сам; пустая строка — легитимно "леджера
// ещё нет" (см. common_prolog выше, тот же смысл).
std::optional<std::string> auth_and_ledger(AppState& state, const httplib::Request& req,
                                            httplib::Response& res) {
    auto user = session::current_user(req, *state.jwks);
    if (!user) {
        json_error(res, 401, "unauthenticated");
        return std::nullopt;
    }
    auto subject_id = resolve_subject_user_id(*user, req, res);
    if (!subject_id) return std::nullopt; // resolve_subject_user_id уже ответил (403)
    auto ledger_id = reports_repo::find_ledger_for_user(*state.reports_pool->acquire(), *subject_id);
    return ledger_id.value_or(std::string{});
}

std::optional<Context> common_prolog(AppState& state, const httplib::Request& req,
                                      httplib::Response& res) {
    auto user = session::current_user(req, *state.jwks);
    if (!user) {
        json_error(res, 401, "unauthenticated");
        return std::nullopt;
    }
    auto subject_id = resolve_subject_user_id(*user, req, res);
    if (!subject_id) return std::nullopt; // resolve_subject_user_id уже ответил (403)

    if (!req.has_param("eq_currency") || req.get_param_value("eq_currency").empty()) {
        json_error(res, 400, "eq_currency обязателен (ISO-код валюты эквивалента, например USD)");
        return std::nullopt;
    }
    std::string eq_currency_iso = req.get_param_value("eq_currency");

    Context ctx;
    auto ledger_id = reports_repo::find_ledger_for_user(*state.reports_pool->acquire(), *subject_id);
    ctx.ledger_id = ledger_id.value_or(std::string{});

    // Резолвим eq_currency, только если у пользователя вообще есть леджер —
    // без него нет и "своей" копии валюты, а искать только среди общих не
    // ошибка, но и незачем: результат отчёта будет пуст в любом случае.
    if (!ctx.ledger_id.empty()) {
        auto eq_uuid = reports_repo::resolve_eq_currency(*state.reports_pool->acquire(), eq_currency_iso,
                                                          ctx.ledger_id);
        if (!eq_uuid) {
            json_error(res, 400, "неизвестный ISO-код валюты эквивалента: " + eq_currency_iso);
            return std::nullopt;
        }
        ctx.eq_currency_uuid = *eq_uuid;
    }
    return ctx;
}

// eq_currency_uuid — не из ReportResult (там его нет, reports_repo его
// только принимает, а не запоминает), а из уже разрешённого Context: клиенту
// он нужен, чтобы отличить "кошелёк уже в валюте эквивалента" (currency_uuid
// строки == eq_currency_uuid) от "пересчитан по курсу" — см. android-wallet/docs/ARCHITECTURE.md,
// "Три состояния эквивалента" (та же разметка ≈/~, что и в Android-приложении).
json result_to_json(const reports_repo::ReportResult& result, const std::string& eq_currency_uuid) {
    return {{"accounts", result.rows},
            {"total_eq", result.total_eq},
            {"incomplete", result.incomplete},
            {"eq_currency_uuid", eq_currency_uuid}};
}

void handle_balance(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/balance"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("date") || req.get_param_value("date").empty()) {
        json_error(res, 400, "date обязателен");
        return;
    }

    reports_repo::ReportResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::balance_to_date(*state.reports_pool->acquire(), ctx->ledger_id,
                                                req.get_param_value("date"), ctx->eq_currency_uuid);
    }
    res.set_content(result_to_json(result, ctx->eq_currency_uuid).dump(), kJsonContentType);
}

void handle_income(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/income"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    reports_repo::ReportResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::income_for_period(*state.reports_pool->acquire(), ctx->ledger_id,
                                                  req.get_param_value("from"),
                                                  req.get_param_value("to"), ctx->eq_currency_uuid);
    }
    res.set_content(result_to_json(result, ctx->eq_currency_uuid).dump(), kJsonContentType);
}

void handle_expense(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/expense"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    reports_repo::ReportResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::expense_for_period(*state.reports_pool->acquire(), ctx->ledger_id,
                                                   req.get_param_value("from"),
                                                   req.get_param_value("to"), ctx->eq_currency_uuid);
    }
    res.set_content(result_to_json(result, ctx->eq_currency_uuid).dump(), kJsonContentType);
}

// "Структура расходов" (Design/.../Отчёты - веб.dc.html, экран "структура") —
// доля каждой категории в расходе за период, с необязательным сравнением
// периодов (cmp_from/cmp_to). В отличие от /balance,/income,/expense
// (result_to_json + ReportResult) — свой формат ответа (StructureResult),
// поэтому не переиспользует result_to_json выше.
void handle_expense_structure(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/expense-structure"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    // cmp_from/cmp_to — только вместе или ни одного: одна граница периода
    // сравнения без другой сервису решить нечем (а сам "предыдущий
    // период"/"год назад" против ЧЕГО считать — забота вызывающего, см.
    // openapi.yaml).
    bool has_cmp_from = req.has_param("cmp_from") && !req.get_param_value("cmp_from").empty();
    bool has_cmp_to = req.has_param("cmp_to") && !req.get_param_value("cmp_to").empty();
    if (has_cmp_from != has_cmp_to) {
        json_error(res, 400, "cmp_from и cmp_to указываются только вместе");
        return;
    }
    std::optional<std::string> cmp_from, cmp_to;
    if (has_cmp_from) {
        cmp_from = req.get_param_value("cmp_from");
        cmp_to = req.get_param_value("cmp_to");
    }

    reports_repo::StructureResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::expense_structure_for_period(
            *state.reports_pool->acquire(), ctx->ledger_id, req.get_param_value("from"), req.get_param_value("to"),
            ctx->eq_currency_uuid, cmp_from, cmp_to);
    }

    json body = {
        {"categories", result.categories},
        {"total_eq", result.total_eq},
        {"incomplete", result.incomplete},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
        {"compared", result.compared},
    };
    if (result.compared) body["total_eq_before"] = result.total_eq_before;
    res.set_content(body.dump(), kJsonContentType);
}

// Расход «категория × месяц» (change add-category-month-analytics, первый
// потребитель — wallet-mcp): те же from/to/eq_currency и те же 400 до базы,
// что у /expense-structure; правила группировки — тоже её (ноги перевода
// погашены в Transfer), поэтому итоги совпадают со структурой, а не с
// /income-expense-by-month. Без леджера — пустой ответ без запроса (months
// тоже пуст: список месяцев строит база).
void handle_expense_by_category_by_month(AppState& state, const httplib::Request& req,
                                         httplib::Response& res) {
    applog::set_route("GET /api/reports/expense-by-category-by-month"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    reports_repo::CategoryByMonthResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::expense_by_category_by_month(*state.reports_pool->acquire(), ctx->ledger_id,
                                                            req.get_param_value("from"),
                                                            req.get_param_value("to"), ctx->eq_currency_uuid);
    }
    json body = {
        {"months", result.months},
        {"categories", result.categories},
        {"total_eq", result.total_eq},
        {"incomplete", result.incomplete},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    res.set_content(body.dump(), kJsonContentType);
}

// "Обзор" (Design/.../Отчёты - веб.dc.html, экран "обзор") — три отдельные
// панели, три отдельных хендлера ниже: KPI-плитки и мини-"Структура" самого
// экрана "Обзор" на клиенте переиспользуют /balance,/income,/expense,
// /expense-structure целиком, отдельных ручек под них заводить не нужно.

void handle_income_expense_by_month(AppState& state, const httplib::Request& req,
                                     httplib::Response& res) {
    applog::set_route("GET /api/reports/income-expense-by-month"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    reports_repo::MonthlyResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::income_expense_by_month(*state.reports_pool->acquire(), ctx->ledger_id,
                                                         req.get_param_value("from"),
                                                         req.get_param_value("to"), ctx->eq_currency_uuid);
    }
    json body = {
        {"months", result.months},
        {"incomplete", result.incomplete},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    res.set_content(body.dump(), kJsonContentType);
}

void handle_expense_by_weekday(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/expense-by-weekday"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    reports_repo::WeekdayResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::expense_by_weekday(*state.reports_pool->acquire(), ctx->ledger_id,
                                                   req.get_param_value("from"), req.get_param_value("to"),
                                                   ctx->eq_currency_uuid);
    }
    json body = {
        {"days", result.days},
        {"incomplete", result.incomplete},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    res.set_content(body.dump(), kJsonContentType);
}

void handle_expense_biggest_moves(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/expense-biggest-moves"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    // В отличие от /expense-structure — здесь cmp_from/cmp_to ОБЯЗАТЕЛЬНЫ:
    // без периода сравнения самой панели "что изменилось" нечего показывать.
    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty() ||
        !req.has_param("cmp_from") || req.get_param_value("cmp_from").empty() ||
        !req.has_param("cmp_to") || req.get_param_value("cmp_to").empty()) {
        json_error(res, 400, "from, to, cmp_from и cmp_to обязательны");
        return;
    }

    // limit — сколько строк показать (design — 6 плиток); зажимаем в
    // разумных границах, а не доверяем клиенту произвольное число.
    int limit = 6;
    if (req.has_param("limit") && !req.get_param_value("limit").empty()) {
        try {
            limit = std::stoi(req.get_param_value("limit"));
        } catch (const std::exception&) {
            json_error(res, 400, "limit должен быть числом");
            return;
        }
    }
    if (limit < 1 || limit > 20) {
        json_error(res, 400, "limit должен быть от 1 до 20");
        return;
    }

    reports_repo::BiggestMovesResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::expense_biggest_moves(
            *state.reports_pool->acquire(), ctx->ledger_id, req.get_param_value("from"), req.get_param_value("to"),
            req.get_param_value("cmp_from"), req.get_param_value("cmp_to"), ctx->eq_currency_uuid, limit);
    }
    json body = {
        {"rows", result.rows},
        {"incomplete", result.incomplete},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    res.set_content(body.dump(), kJsonContentType);
}

// "Список операций" (Design/.../Отчёты - веб (офлайн).html, вид 'ops') —
// ОДНА страница операций периода (курсорная пагинация, см.
// reports_repo.hpp у operations_for_period). Необязательные фильтры
// category_uuid/wallet_uuid/kind/q (change add-operations-filters) и
// exclude_category_uuid (add-category-month-analytics) — до
// пагинации, в SQL; при хоть одном фильтре в ответе итог total_eq/
// total_incomplete/total_count по всем подходящим операциям периода.
void handle_operations(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/operations"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    // limit — по умолчанию 1000 (по прямому запросу — "если выбираем 20 лет,
    // то нет необходимости доставать всё"), 1..1000: верхняя граница защищает
    // от произвольно большого "дай мне сразу всё" через query-параметр — тот
    // же принцип, что и у limit в expense-biggest-moves, просто диапазон
    // здесь шире (страница операций по смыслу крупнее топ-N категорий).
    int limit = 1000;
    if (req.has_param("limit") && !req.get_param_value("limit").empty()) {
        try {
            limit = std::stoi(req.get_param_value("limit"));
        } catch (const std::exception&) {
            json_error(res, 400, "limit должен быть числом");
            return;
        }
    }
    if (limit < 1 || limit > 1000) {
        json_error(res, 400, "limit должен быть от 1 до 1000");
        return;
    }

    // cursor_date/cursor_uuid — оба или ни одного (тот же принцип, что у
    // cmp_from/cmp_to в expense-structure): смешанный случай — ошибка
    // клиента (сломанная страница), а не молчаливое "как будто первая
    // страница".
    std::string cursor_date = req.get_param_value("cursor_date");
    std::string cursor_uuid = req.get_param_value("cursor_uuid");
    if (cursor_date.empty() != cursor_uuid.empty()) {
        json_error(res, 400, "cursor_date и cursor_uuid — оба или ни одного");
        return;
    }

    // Фильтры — после курсора (design D2: прежние сценарии отвечают прежними
    // текстами) и до запроса ленты, чтобы ошибка ввода не стала 500 на
    // приведении ::uuid[] в Postgres.
    // q повторяемый (search-text-variants): q=стриж&q=barber — комментарий
    // с ЛЮБЫМ из вариантов. Один q — как раньше.
    std::vector<std::string> q_values;
    for (std::size_t i = 0; i < req.get_param_value_count("q"); ++i) q_values.push_back(req.get_param_value("q", i));
    reports_repo::OperationsFilter filter;
    if (auto err = reports_repo::parse_operations_filter(
            req.get_param_value("category_uuid"), req.get_param_value("exclude_category_uuid"),
            req.get_param_value("wallet_uuid"),
            req.get_param_value("kind"), q_values, filter)) {
        json_error(res, 400, *err);
        return;
    }

    reports_repo::OperationsResult result;
    if (!ctx->ledger_id.empty()) {
        // Страница и итог — на одном соединении из пула (одна аренда).
        result = reports_repo::operations_for_period(*state.reports_pool->acquire(), ctx->ledger_id,
                                                       req.get_param_value("from"),
                                                       req.get_param_value("to"), ctx->eq_currency_uuid,
                                                       limit, cursor_date, cursor_uuid, filter);
    } else if (filter.any()) {
        // Нет леджера — итог "0"/false/0 без запроса (значения по умолчанию).
        result.has_total = true;
    }
    json body = {
        {"operations", result.rows},
        {"incomplete", result.incomplete},
        {"has_more", result.has_more},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    // Итог — только при фильтрах: без них ответ тот же, что до фильтров.
    if (result.has_total) {
        body["total_eq"] = result.total_eq;
        body["total_incomplete"] = result.total_incomplete;
        body["total_count"] = result.total_count;
    }
    // next_cursor_date/next_cursor_uuid — только когда есть следующая
    // страница (has_more): пустые поля вместо отсутствующих ключей путали бы
    // клиента ("пустая строка — это валидный курсор?" — нет, следующая
    // страница просто не отправила бы $6/$7 вовсе, но клиенту это знать не
    // нужно, у него самого нет доступа к "два текста запроса", см. .cpp).
    if (result.has_more) {
        body["next_cursor_date"] = result.next_cursor_date;
        body["next_cursor_uuid"] = result.next_cursor_uuid;
    }
    res.set_content(body.dump(), kJsonContentType);
}

// "Кошельки и остатки" (Design/.../Отчёты - веб (офлайн).html, вид 'wal') —
// оборот КАЖДОГО кошелька леджера за период. common_prolog (а не
// auth_and_ledger, как раньше) — eq_currency здесь ТЕПЕРЬ нужен (по прямому
// запросу: сортировка "Пришло"/"Ушло" обязана идти по эквиваленту, суммы
// разных валют кошельков иначе несравнимы). Остатки на начало/конец периода
// клиент по-прежнему берёт отдельно — из GET /api/reports/balance-at-dates
// (см. reports_repo.hpp у balances_at_dates).
void handle_wallet_turnover(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/wallet-turnover"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }

    reports_repo::WalletTurnoverResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::wallet_turnover_for_period(*state.reports_pool->acquire(), ctx->ledger_id,
                                                            req.get_param_value("from"),
                                                            req.get_param_value("to"),
                                                            ctx->eq_currency_uuid);
    }
    json body = {
        {"wallets", result.rows},
        {"incomplete", result.incomplete},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    res.set_content(body.dump(), kJsonContentType);
}

// Батч-баланс на несколько дат (по прямому запросу — "сервер научится
// отдавать балком за период для нескольких кошельков"): "Кошельки и
// остатки" делали до 14 отдельных GET /api/reports/balance на один показ
// экрана (начало периода, конец периода, 12 точек спарклайна), "Обзор" — ещё
// 12 на линию остатка "Расхода по месяцам". Обёртка над f_balance_to_dates —
// та же формула, что у /balance, просто сразу на список дат.
//
// dates — через запятую в одном query-параметре (не повторяющийся параметр
// ?dates=...&dates=...): httplib отдаёт только последнее значение
// повторяющегося параметра через get_param_value без индекса, а разбирать
// повтор с индексом ради одного этого эндпоинта было бы лишней возню — простой
// split куда прозрачнее. Лимит 400 — сильно больше практической нужды экрана
// (14), но не безлимитно: дата — не тяжёлый параметр, но f_balance_to_dates
// кросс-джойнит их со всеми кошельками леджера, и без разумного потолка
// запрос мог бы попросить посчитать что угодно.
void handle_balance_at_dates(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/balance-at-dates"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("dates") || req.get_param_value("dates").empty()) {
        json_error(res, 400, "dates обязателен (список дат через запятую)");
        return;
    }
    std::vector<std::string> dates;
    {
        std::string raw = req.get_param_value("dates");
        std::size_t pos = 0;
        while (pos <= raw.size()) {
            std::size_t comma = raw.find(',', pos);
            std::string piece = raw.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (piece.empty()) {
                json_error(res, 400, "dates содержит пустой элемент");
                return;
            }
            dates.push_back(piece);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    if (dates.size() > 400) {
        json_error(res, 400, "dates: не больше 400 дат за один запрос");
        return;
    }

    reports_repo::BalancesAtDatesResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::balances_at_dates(*state.reports_pool->acquire(), ctx->ledger_id, dates,
                                                  ctx->eq_currency_uuid);
    } else {
        // Леджера нет — тот же контракт, что и у остальных отчётов (200,
        // пустой результат, не 404): ровно один пустой блок на каждую дату,
        // а не пустой список вовсе, чтобы вызывающий код видел ту же форму
        // ответа независимо от того, есть ли у пользователя леджер.
        for (const auto& d : dates) {
            result.dates.push_back(
                {{"date", d}, {"accounts", json::array()}, {"total_eq", "0"}, {"incomplete", false}});
        }
    }
    json body = {
        {"dates", result.dates},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    res.set_content(body.dump(), kJsonContentType);
}

// "Обзор"/"Динамика" (по прямому запросу, 2026-09-11 — "новое API, которое
// просит с/по и шагом, а не вызов с миллионом параметров в GET") — та же
// формула баланса, что и у /balance-at-dates выше, но список дат-границ
// бакетов сервер строит САМ из from/to/step (см. reports_repo::balance_series
// и f_balance_series.sql), а не принимает готовым от клиента. Раньше клиент
// сам считал границы бакетов (день/неделя/месяц) и слал их списком в
// query-параметре dates — при широком периоде с группировкой по дням/неделям
// список разрастался до сотен элементов и упирался в лимит длины URI на
// wallet-gateway ("Failed to fetch"); здесь запрос всегда фиксированного
// размера (from+to+step+eq_currency), независимо от длины периода.
void handle_balance_series(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/balance-series"); // литерал из register_routes ниже
    auto ctx = common_prolog(state, req, res);
    if (!ctx) return;

    if (!req.has_param("from") || req.get_param_value("from").empty() ||
        !req.has_param("to") || req.get_param_value("to").empty()) {
        json_error(res, 400, "from и to обязательны");
        return;
    }
    if (!req.has_param("step") || req.get_param_value("step").empty()) {
        json_error(res, 400, "step обязателен (day/week/month/year)");
        return;
    }
    std::string step = req.get_param_value("step");
    if (step != "day" && step != "week" && step != "month" && step != "year") {
        json_error(res, 400, "step должен быть day, week, month или year");
        return;
    }

    // accounts=true — детализация по счетам внутри каждого бакета (см.
    // reports_repo::balance_series). По умолчанию выключена: веб читает
    // только total_eq/incomplete, а детализация за два года по дням — это
    // ~6.5 МБ JSON и десятки МБ пиковой памяти процесса на один запрос.
    std::string accounts_param = req.get_param_value("accounts");
    bool with_accounts = accounts_param == "true" || accounts_param == "1";

    reports_repo::BalanceSeriesResult result;
    if (!ctx->ledger_id.empty()) {
        result = reports_repo::balance_series(*state.reports_pool->acquire(), ctx->ledger_id,
                                               req.get_param_value("from"), req.get_param_value("to"),
                                               step, ctx->eq_currency_uuid, with_accounts);
    }
    // Леджера нет — buckets: [] целиком, не по одному пустому блоку на
    // бакет, как у /balance-at-dates: там пустые блоки предзаполнял вызывающий
    // код по СВОЕМУ списку dates, здесь границы бакетов знает только сама
    // f_balance_series — звать её ради заведомо пустого леджера незачем, а
    // дублировать в C++ её же арифметику дня/недели/месяца/года — тем более.
    // Клиент это уже переживает: без ledger'а и остальные отчёты (Обзор,
    // Категории) пусты, графику попросту нечего показывать.
    json body = {
        {"buckets", result.buckets},
        {"eq_currency_uuid", ctx->eq_currency_uuid},
    };
    res.set_content(body.dump(), kJsonContentType);
}

// Справочник валют (Design/.../Отчёты - веб.dc.html) — веб-клиент строит из
// него выпадающий список "Валюта эквивалента" вместо свободного текстового
// ввода ISO-кода руками (по прямому запросу). Без from/to/eq_currency —
// список валют не зависит от периода, да и сам он источник для eq_currency,
// а не потребитель (см. auth_and_ledger выше, почему свой, более лёгкий
// пролог).
void handle_currencies(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/currencies"); // литерал из register_routes ниже
    auto ledger_id = auth_and_ledger(state, req, res);
    if (!ledger_id) return;

    auto result = reports_repo::list_currencies(*state.reports_pool->acquire(), *ledger_id);
    res.set_content(json{{"currencies", result.currencies}}.dump(), kJsonContentType);
}

// Справочник кошельков и категорий (org/docs/TODO_telegram_bot.md, фаза 1,
// п. 7) — для Telegram-бота: сопоставить имя из сообщения с uuid и показать
// /categories. Как и /currencies, не зависит ни от периода, ни от
// eq_currency, поэтому пролог — auth_and_ledger. Леджера нет — 200 и пустой
// список, тот же контракт, что у остальных отчётов.
void handle_accounts(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/accounts"); // литерал из register_routes ниже
    auto ledger_id = auth_and_ledger(state, req, res);
    if (!ledger_id) return;

    reports_repo::AccountKindFilter kind = reports_repo::AccountKindFilter::Any;
    std::string kind_param = req.get_param_value("kind");
    if (kind_param == "wallet") {
        kind = reports_repo::AccountKindFilter::Wallet;
    } else if (kind_param == "category") {
        kind = reports_repo::AccountKindFilter::Category;
    } else if (!kind_param.empty()) {
        json_error(res, 400, "kind должен быть wallet или category");
        return;
    }

    std::string archived_param = req.get_param_value("include_archived");
    if (!archived_param.empty() && archived_param != "true" && archived_param != "false") {
        json_error(res, 400, "include_archived должен быть true или false");
        return;
    }
    bool include_archived = archived_param == "true";

    reports_repo::AccountsResult result;
    if (!ledger_id->empty()) {
        result = reports_repo::list_accounts(*state.reports_pool->acquire(), *ledger_id, kind, include_archived);
    }
    res.set_content(json{{"accounts", result.accounts}}.dump(), kJsonContentType);
}

// Иконки, доступные ЭТОМУ пользователю (вкладка "Иконки" в "Отчётах",
// wallet-web app.js — по прямому запросу, 2026-09-22): общий набор плюс свои
// иконки его леджера — тот же list_icons, что и у handle_admin_icons ниже,
// только леджер — СВОЙ (через auth_and_ledger, с as_user_id для "Посмотреть
// как клиент"), а не любого user_id на выбор. Леджера ещё нет — не ошибка:
// общий набор виден и без него, user: [].
void handle_icons(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/icons"); // литерал из register_routes ниже
    auto ledger_id = auth_and_ledger(state, req, res);
    if (!ledger_id) return;

    std::optional<std::string> own_ledger;
    if (!ledger_id->empty()) own_ledger = *ledger_id;
    auto result = reports_repo::list_icons(*state.reports_pool->acquire(), own_ledger);
    res.set_content(json{{"common", result.common}, {"user", result.user}}.dump(), kJsonContentType);
}

// Админ-таблица (wallet-web/site/app.js::renderAdmin()) — агрегаты по
// пользователям, которых SPA уже получила из google-auth GET /admin/users:
// число кошельков, число операций, даты первой/последней операции, число
// участников леджера (см. reports_repo::list_user_stats). Только Role::Admin —
// тот же принцип, что и у списка пользователей в google-auth (support туда
// тоже не допущен, см. его admin.cpp).
//
// POST с телом {"user_ids": [...]}, а не GET: считается только по
// переданным id. Прежний GET /api/reports/admin/users считал по ВСЕМ
// владельцам леджеров базы — на стенде с данными нагрузочного теста это
// 4181 леджер, 15,8 с и 1 МБ JSON ради двух строк таблицы. Список id в теле,
// а не в query: на сотнях пользователей URL упёрся бы в лимиты прокси.
//
// Путь — под /api/reports/*, а не /admin/*: остаётся на этом сервисе без
// правки маршрутизации wallet-gateway (см. его README, "Порядок важен" —
// /admin/* целиком уходит на google-auth).
constexpr size_t kMaxStatsUserIds = 5000;

bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

void handle_admin_user_stats(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("POST /api/reports/admin/users/stats"); // литерал из register_routes ниже
    auto user = session::current_user(req, *state.jwks);
    if (!user) {
        json_error(res, 401, "unauthenticated");
        return;
    }
    if (user->role != "admin") {
        json_error(res, 403, "forbidden");
        return;
    }

    json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (!body.is_object() || !body.contains("user_ids") || !body["user_ids"].is_array()) {
        json_error(res, 400, "ожидается тело {\"user_ids\": [uuid, ...]}");
        return;
    }
    if (body["user_ids"].size() > kMaxStatsUserIds) {
        json_error(res, 400, "user_ids: не больше " + std::to_string(kMaxStatsUserIds));
        return;
    }
    std::vector<std::string> user_ids;
    for (const auto& id : body["user_ids"]) {
        if (!id.is_string() || !is_uuid(id.get<std::string>())) {
            json_error(res, 400, "user_ids: каждый элемент — UUID строкой");
            return;
        }
        user_ids.push_back(id.get<std::string>());
    }

    auto result = reports_repo::list_user_stats(*state.reports_pool->acquire(), user_ids);
    res.set_content(json{{"users", result.users}}.dump(), kJsonContentType);
}

// Иконки (по прямому запросу, 2026-09-16 — "общий блок со всеми общими
// иконками, и выбор пользователя, чтобы посмотреть его личные") — тот же
// admin-only принцип, что и у handle_admin_user_stats выше. Необязательный
// query-параметр user_id: без него — только общий/эталонный набор, с ним —
// плюс свои иконки ЛЕДЖЕРА этого пользователя (через find_ledger_for_user,
// не user_id напрямую — см. её комментарий про "один леджер на
// пользователя" для MVP).
void handle_admin_icons(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/reports/admin/icons"); // литерал из register_routes ниже
    auto user = session::current_user(req, *state.jwks);
    if (!user) {
        json_error(res, 401, "unauthenticated");
        return;
    }
    if (user->role != "admin") {
        json_error(res, 403, "forbidden");
        return;
    }

    std::optional<std::string> ledger_id;
    if (req.has_param("user_id") && !req.get_param_value("user_id").empty()) {
        ledger_id = reports_repo::find_ledger_for_user(*state.reports_pool->acquire(),
                                                        req.get_param_value("user_id"));
        if (!ledger_id) {
            json_error(res, 404, "у пользователя ещё нет леджера");
            return;
        }
    }

    auto result = reports_repo::list_icons(*state.reports_pool->acquire(), ledger_id);
    res.set_content(json{{"common", result.common}, {"user", result.user}}.dump(), kJsonContentType);
}

} // namespace

void register_routes(httplib::Server& svr, AppState& state) {
    svr.Get("/api/reports/balance", [&state](const httplib::Request& req, httplib::Response& res) {
        handle_balance(state, req, res);
    });
    svr.Get("/api/reports/balance-at-dates",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_balance_at_dates(state, req, res);
            });
    svr.Get("/api/reports/balance-series",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_balance_series(state, req, res);
            });
    svr.Get("/api/reports/income", [&state](const httplib::Request& req, httplib::Response& res) {
        handle_income(state, req, res);
    });
    svr.Get("/api/reports/expense", [&state](const httplib::Request& req, httplib::Response& res) {
        handle_expense(state, req, res);
    });
    svr.Get("/api/reports/expense-structure",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_expense_structure(state, req, res);
            });
    svr.Get("/api/reports/expense-by-category-by-month",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_expense_by_category_by_month(state, req, res);
            });
    svr.Get("/api/reports/income-expense-by-month",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_income_expense_by_month(state, req, res);
            });
    svr.Get("/api/reports/expense-by-weekday",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_expense_by_weekday(state, req, res);
            });
    svr.Get("/api/reports/expense-biggest-moves",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_expense_biggest_moves(state, req, res);
            });
    svr.Get("/api/reports/operations", [&state](const httplib::Request& req, httplib::Response& res) {
        handle_operations(state, req, res);
    });
    svr.Get("/api/reports/currencies", [&state](const httplib::Request& req, httplib::Response& res) {
        handle_currencies(state, req, res);
    });
    svr.Get("/api/reports/accounts", [&state](const httplib::Request& req, httplib::Response& res) {
        handle_accounts(state, req, res);
    });
    svr.Get("/api/reports/wallet-turnover",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_wallet_turnover(state, req, res);
            });
    svr.Get("/api/reports/icons", [&state](const httplib::Request& req, httplib::Response& res) {
        handle_icons(state, req, res);
    });
    svr.Post("/api/reports/admin/users/stats",
             [&state](const httplib::Request& req, httplib::Response& res) {
                 handle_admin_user_stats(state, req, res);
             });
    svr.Get("/api/reports/admin/icons",
            [&state](const httplib::Request& req, httplib::Response& res) {
                handle_admin_icons(state, req, res);
            });
}

} // namespace reportsapi
