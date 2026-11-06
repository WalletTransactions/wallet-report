// Логика отчётов (org/docs/TODO_mobile_sync.md, Фаза 4) — отделена от
// HTTP-слоя (reports.hpp/.cpp) тем же приёмом, что sync_repo.hpp отделён от
// sync.hpp: без db::Database в конструкторе это можно проверить
// интеграционным тестом напрямую, не поднимая httplib/JWKS-стек (см.
// tests_integration/test_reports.cpp).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db.hpp"
#include "nlohmann/json.hpp"

namespace reports_repo {

// nullopt — у пользователя ещё нет ни одного леджера (не провизионирует —
// см. reports.hpp, шапка файла, почему это read-only путь).
std::optional<std::string> find_ledger_for_user(db::Database& db, const std::string& user_id);

// ISO-код -> uuid валюты; предпочитает ЛЕДЖЕР-СВОЮ копию общей валюте с тем
// же кодом (тот же приём, что разрешение курса в f_balance_to_date.sql).
// nullopt — валюты с таким ISO-кодом, видимой этому леджеру, не нашлось.
std::optional<std::string> resolve_eq_currency(db::Database& db, const std::string& iso_code,
                                                const std::string& ledger_id);

// "Валюта эквивалента" (GET /api/reports/currencies) — справочник, из
// которого веб-клиент строит выпадающий список вместо свободного ввода
// ISO-кода руками. Те же строки, что видит resolve_eq_currency выше (общие,
// ledger_id IS NULL, плюс свои этого леджера), но списком и с полями,
// нужными для подписи в UI (name/symbol/icon_sha256 — "флаг страны" в
// выпадающем списке, по прямому запросу), а не одна найденная по коду.
//
// ledger_id — пустая строка означает "у пользователя ещё нет леджера" (см.
// find_ledger_for_user); в этом случае видны только общие валюты. В отличие
// от остальных функций файла, вызывать с пустой строкой БЕЗОПАСНО — сама
// функция (см. .cpp) не пытается забиндить пустую строку в параметр,
// типизированный как uuid (Postgres откажется её скастовать), а использует
// под пустой ledger_id отдельный текст запроса без этого параметра вовсе.
struct CurrenciesResult {
    // {uuid, iso_code, name, symbol, icon_sha256, show_in_equivalent (bool),
    // is_own (bool, true — своя валюта леджера), icon_uuid, precision (число)},
    // ORDER BY iso_code. Валюты со снятым show_in_equivalent из списка НЕ
    // убираются — фильтрует клиент (пикер эквивалента), тот же справочник
    // целиком нужен пикерам валютных пар (change currency-ui-tweaks). При
    // совпадении iso_code между общей валютой и своей этого леджера в
    // списке только ОДНА строка — своя (тот же приоритет, что у
    // resolve_eq_currency): без этого пользователь видел бы в выпадающем
    // списке два неотличимых на вид "USD" и не мог бы понять, какой из них —
    // резолвится в "свой". icon_sha256 — hex content_sha256 иконки валюты
    // (currency.icon_uuid NOT NULL, значит есть всегда, см. .cpp) — та же
    // "иконка-флаг" из схемы, что веб-клиент качает через GET
    // /sync/blobs/:sha256, тем же приёмом, что icon_sha256 у категорий
    // (см. ExpenseStructureRow).
    nlohmann::json currencies = nlohmann::json::array();
};

CurrenciesResult list_currencies(db::Database& db, const std::string& ledger_id);

// Полный справочник кошельков и категорий леджера (GET /api/reports/accounts,
// org/docs/TODO_telegram_bot.md, §0 и фаза 1, п. 7) — в отличие от
// expense-structure/wallet-turnover, без привязки к периоду и обороту: боту
// нужно сопоставлять имена из сообщения («350 продукты») со ВСЕМИ живыми
// категориями, а не только с теми, по которым уже были траты.
//
// Кошелёк или категория определяется наличием живой строки account_operating
// (как у v_account_operating и остальных отчётов), а не колонкой
// account.kind: account_operating — источник валюты кошелька, и именно по нему
// сервер делит счета во всех расчётах; kind — лишь его слепок на строке.
enum class AccountKindFilter { Any, Wallet, Category };

struct AccountsResult {
    // {uuid, name, kind ("wallet"/"category"), system_kind (число 0/1/2),
    // is_archived (bool), icon_sha256, currency_uuid, iso_code, symbol,
    // precision}, ORDER BY kind (кошельки первыми), name. Валютные поля —
    // пустые строки у категорий (SQL NULL, см. db.hpp).
    nlohmann::json accounts = nlohmann::json::array();
};

AccountsResult list_accounts(db::Database& db, const std::string& ledger_id, AccountKindFilter kind,
                             bool include_archived);

struct ReportResult {
    nlohmann::json rows = nlohmann::json::array();
    // Текстом, как и приходит из libpq (TEXT result format, см. db.hpp) —
    // тот же принцип "numeric — строка", что и в протоколе синка, просто
    // здесь он достаётся бесплатно: to_jsonb() тут вообще не участвует, это
    // прямой SELECT колонок, libpq и так отдаёт numeric как текст.
    std::string total_eq = "0";
    bool incomplete = false;
};

// Баланс каждого кошелька леджера на дату — обёртка над f_balance_to_date
// (wallet-api/schema/function/f_balance_to_date.sql) с агрегатом (sum/incomplete
// по всему результату через `OVER ()`, без второго похода в базу).
ReportResult balance_to_date(db::Database& db, const std::string& ledger_id,
                              const std::string& date, const std::string& eq_currency_uuid);

// Доход за период с разбивкой по счёту — обёртка над f_income_for_period.
ReportResult income_for_period(db::Database& db, const std::string& ledger_id,
                                const std::string& from, const std::string& to,
                                const std::string& eq_currency_uuid);

// Расход за период с разбивкой по счёту — обёртка над f_expanse_for_period.
ReportResult expense_for_period(db::Database& db, const std::string& ledger_id,
                                 const std::string& from, const std::string& to,
                                 const std::string& eq_currency_uuid);

// Расход за период с разбивкой ТОЛЬКО по категории (не по (категория,
// валюта), как expense_for_period) — обёртка над
// f_expanse_by_category_for_period. Валюты уже смешаны через amount_eq,
// поэтому в rows нет ни "currency_uuid", ни "amount" (своя сумма), только
// "account_uuid"/"account_name"/"amount_eq"/"icon_sha256"/"op_count" (op_count —
// JSON-число операций в сумме строки, у Transfer — число переводов; hex content_sha256 у icon_sha256
// иконки категории — у account.icon_uuid FK NOT NULL, значит есть всегда;
// картинку веб-клиент качает сам через GET /sync/blobs/:sha256, см. blob.hpp
// — сессионной куки достаточно, device_id не нужен). Основа
// expense_structure_for_period ниже, но пригодится и сама по себе, если
// понадобится плоский список без сравнения периодов.
ReportResult expense_by_category_for_period(db::Database& db, const std::string& ledger_id,
                                              const std::string& from, const std::string& to,
                                              const std::string& eq_currency_uuid);

// "Структура расходов" (Design/.../Отчёты - веб.dc.html, экран "структура",
// GET /api/reports/expense-structure) — доля каждой категории в расходе за
// период (rows.pct — строка "24.3", проценты, без "%") плюс, если задан
// период сравнения (cmp_from И cmp_to, оба или ни одного — иначе caller
// обязан отклонить запрос как 400 ДО вызова этой функции), сдвиг доли в
// процентных пунктах (rows.pct_before/rows.pp_delta). Категория, которая
// была в cmp-периоде, но пропала в основном, тоже попадает в список
// (amount_eq "0", pp_delta отрицательный) — иначе "исчезнувшая" категория
// была бы не видна в сравнении вообще, хотя именно её и интересно показать.
//
// pct/pct_before/pp_delta — JSON null, если соответствующий процент нельзя
// посчитать: либо amount_eq самой строки не определён (курса не нашлось —
// тот же смысл "~", что и в incomplete), либо знаменатель (total_eq за
// соответствующий период) равен нулю (расходов не было вовсе).
//
// В отличие от ReportResult.total_eq (и всех остальных numeric-полей
// проекта, которые каждый раз обходят FP умышленно — см. README wallet-api/schema,
// "Деньги — numeric"), pct/pp_delta — это НЕ деньги: производная величина
// только для интерфейса, никогда не хранится и не суммируется дальше,
// поэтому посчитана через double (см. .cpp) без нарушения того принципа.
struct StructureResult {
    nlohmann::json categories = nlohmann::json::array();
    std::string total_eq = "0";
    std::string total_eq_before = "0"; // осмысленно только при compared == true
    bool incomplete = false;
    bool compared = false;
};

StructureResult expense_structure_for_period(db::Database& db, const std::string& ledger_id,
                                              const std::string& from, const std::string& to,
                                              const std::string& eq_currency_uuid,
                                              const std::optional<std::string>& cmp_from,
                                              const std::optional<std::string>& cmp_to);

// Расход «категория × календарный месяц» (GET /api/reports/expense-by-category-by-month,
// change add-category-month-analytics) — обёртка над f_expense_by_category_by_month.
// Правила — те же, что у /expense-structure (ноги перевода погашены в Transfer),
// поэтому amount_eq категории и total_eq равны полям структуры за тот же период.
//
// months — ВСЕ календарные месяцы периода строками "YYYY-MM-DD" (generate_series
// от date_trunc('month', from) до to, в базе); у категории в её months — только
// непустые клетки. Все суммы и итоги — numeric из базы текстом (design D2): ни
// одна сумма не складывается в C++.
//
// categories[i]: {account_uuid, account_name, icon_sha256, amount_eq (итог за
// период; null — ни у одной операции нет курса), op_count (число),
// months: [{month_start, amount_eq, op_count, incomplete}]}.
struct CategoryByMonthResult {
    nlohmann::json months = nlohmann::json::array();
    nlohmann::json categories = nlohmann::json::array();
    std::string total_eq = "0";
    bool incomplete = false;
};

CategoryByMonthResult expense_by_category_by_month(db::Database& db, const std::string& ledger_id,
                                                   const std::string& from, const std::string& to,
                                                   const std::string& eq_currency_uuid);

// "Обзор" (Design/.../Отчёты - веб.dc.html, экран "обзор", по прямому
// запросу — без групп категорий и без "обязательных платежей", их
// пользователь явно исключил). Три отдельные функции ниже — по одной на
// панель экрана; KPI-плитки и мини-"Структура" в "Обзоре" переиспользуют
// уже существующие balance_to_date/income_for_period/expense_for_period/
// expense_structure_for_period целиком, отдельной функции под них не нужно.

// "Расход по месяцам" — обёртка над f_income_expense_by_month. months —
// РОВНО по одному элементу на каждый календарный месяц между from и to
// (включая месяцы без единой операции — see .sql, зачем именно generate_series,
// а не голый GROUP BY), поэтому дальше НЕ нужно ничего дозаполнять на
// стороне вызывающего кода.
struct MonthlyResult {
    nlohmann::json months = nlohmann::json::array(); // {month_start, income_eq, expense_eq}
    bool incomplete = false;
};

MonthlyResult income_expense_by_month(db::Database& db, const std::string& ledger_id,
                                       const std::string& from, const std::string& to,
                                       const std::string& eq_currency_uuid);

// "По дням недели" — обёртка над f_expense_by_weekday_for_period. days —
// РОВНО 7 элементов, isodow 1..7 (понедельник..воскресенье), зануленные дни
// без расходов включены (тот же принцип, что и у months выше).
struct WeekdayResult {
    nlohmann::json days = nlohmann::json::array(); // {isodow, expense_eq}
    bool incomplete = false;
};

WeekdayResult expense_by_weekday(db::Database& db, const std::string& ledger_id,
                                  const std::string& from, const std::string& to,
                                  const std::string& eq_currency_uuid);

// "Что изменилось сильнее всего" — топ категорий по МОДУЛЮ разницы amount_eq
// между текущим периодом (from/to) и периодом сравнения (cmp_from/cmp_to,
// оба обязательны — в отличие от expense_structure_for_period, здесь без
// сравнения самой панели просто нечего показывать). В отличие от pct/pp_delta
// в StructureResult (проценты, не деньги — см. её комментарий), amount_now_eq/
// amount_before_eq здесь — НАСТОЯЩИЕ денежные строки как есть из SQL,
// double участвует ТОЛЬКО во внутренней сортировке по |before-now| (никогда
// не сериализуется, не форматируется как отдельное значение) — сам "дельта"
// в деньгах на экране считает клиент из этих двух строк, тем же приёмом,
// что и pp_delta на клиенте "Структуры".
//
// Категория, у которой amount_eq неизвестен (курса нет) хотя бы в одном из
// двух периодов, в список не попадает — дельту от неизвестного числа
// посчитать нельзя, а молча считать её нулём значило бы придумать
// несуществующее изменение. Категория, которой не было в ОДНОМ из периодов
// целиком (не 0, а просто нет такой строки), — это честный 0 для него, тем
// же принципом, что "исчезнувшая"/"появившаяся" категория в
// expense_structure_for_period.
struct BiggestMovesResult {
    // {account_uuid, account_name, icon_sha256, amount_now_eq, amount_before_eq},
    // отсортировано по |amount_now_eq - amount_before_eq| убыв., не больше limit строк.
    nlohmann::json rows = nlohmann::json::array();
    bool incomplete = false;
};

BiggestMovesResult expense_biggest_moves(db::Database& db, const std::string& ledger_id,
                                          const std::string& from, const std::string& to,
                                          const std::string& cmp_from, const std::string& cmp_to,
                                          const std::string& eq_currency_uuid, int limit);

// "Список операций" (Design/.../Отчёты - веб (офлайн).html, вид 'ops',
// GET /api/reports/operations) — страница операций периода, сортировка по
// дате убыв. Обёртка над f_operations_for_period. Необязательные фильтры
// (OperationsFilter ниже, change add-operations-filters) применяются в самой
// SQL-функции ДО курсора и LIMIT — поэтому has_more/курсор относятся к
// отфильтрованному набору. Веб и бот пока фильтруют загруженные страницы
// сами; серверные фильтры нужны wallet-mcp (search_operations).
//
// Курсорная пагинация — по прямому запросу (2026-09-12, "давай улучшим
// операции... по умолчанию 1000, если выбираем 20 лет — не тащить всё"):
// раньше отдавала ВЕСЬ период одним запросом (см. историю этого файла) —
// для активного пользователя за много лет это тысячи строк с джойнами на
// каждый показ "Обзора"/"Динамики"/"Категорий" (они читают этот же эндпоинт
// целиком для клиентской агрегации — см. app.js::fetchAllOperations,
// прозрачно листающую страницы в цикле, раз им нужно ВСЁ окно периода для
// сумм) и особенно самого "Списка операций" (там чаще всего смотрят только
// первую страницу — см. app.js, ленивая подгрузка по доскроллу).
//
// rows[i].kind — "expense"/"income"/"transfer" (см. .sql про то, как
// разновалютный перевод через категорию Transfer естественно попадает как
// expense/income, а не отдельным случаем). category_uuid/category_name/
// icon_sha256/wallet_uuid/wallet_name — пустая строка (SQL NULL, см.
// db.hpp) у transfer-строк: там участвуют оба кошелька (from_uuid/to_uuid),
// а не один кошелёк и категория.
struct OperationsResult {
    nlohmann::json rows = nlohmann::json::array();
    // true — хотя бы у одной операции ЭТОЙ СТРАНИЦЫ не нашлось курса в
    // eq_currency (её amount_eq — пустая строка, тот же смысл "~", что и
    // incomplete у остальных отчётов) — ПО СТРАНИЦЕ, не по всему периоду
    // целиком: с пагинацией иначе и нельзя, тот же принцип, что уже у
    // incomplete отдельного бакета в f_balance_series.
    bool incomplete = false;
    // true — страница получилась ровно limit строк ИЗ limit+1 запрошенных
    // "с запасом" (см. .cpp) — значит после неё есть ещё хотя бы одна
    // операция; next_cursor_date/next_cursor_uuid тогда — (date, uuid)
    // последней ОТДАННОЙ строки, эхом в cursor_date/cursor_uuid следующего
    // вызова. Пусты, если has_more == false (страница последняя).
    bool has_more = false;
    std::string next_cursor_date;
    std::string next_cursor_uuid;
    // Итог по фильтру — только если задан хоть один фильтр (has_total). По
    // ВСЕМ подходящим операциям периода, без курсора и лимита (см.
    // operations_total). total_eq — numeric из базы текстом как есть.
    bool has_total = false;
    std::string total_eq = "0";
    bool total_incomplete = false;
    long long total_count = 0;
};

// Фильтры ленты (change add-operations-filters). Пустой вектор/строка —
// фильтр НЕ задан (в SQL параметр не передаётся вовсе и получает DEFAULT
// NULL). Пустого списка «ни одной категории» отсюда не бывает: пустое
// значение query-параметра равносильно его отсутствию. uuid в списках
// обязаны быть уже проверены (parse_operations_filter): из них собирается
// литерал массива '{a,b}', и битый uuid дал бы ошибку Postgres (500), а не 400.
struct OperationsFilter {
    std::vector<std::string> category_uuids; // ИЛИ внутри списка; только неоперационная сторона
    // Исключение (add-category-month-analytics): операции этих категорий (та же
    // неоперационная сторона) отбрасываются; прямой перевод кошелёк → кошелёк
    // остаётся. С category_uuids вместе не задаётся (400 в parse_operations_filter).
    std::vector<std::string> exclude_category_uuids;
    std::vector<std::string> wallet_uuids;   // ИЛИ; операционная сторона, прямой перевод — по любому из двух
    std::string kind;                        // "", "expense", "income", "transfer"
    std::string q;                           // подстрока комментария, без регистра, ё = е, буквально
    // Несколько вариантов подстроки (search-text-variants): комментарий
    // подходит, если содержит ЛЮБОЙ из них. Один вариант идёт в q (p_q — тот же
    // вызов, что до миграции wallet-api 000032), два и больше — сюда (p_qs).
    // parse_operations_filter не заполняет q и qs одновременно.
    std::vector<std::string> qs;

    bool any() const {
        return !category_uuids.empty() || !exclude_category_uuids.empty() || !wallet_uuids.empty() ||
               !kind.empty() || !q.empty() || !qs.empty();
    }
};

// Разбор и проверка query-параметров фильтра — чистая функция без базы и
// httplib (её зовут handle_operations и интеграционные тесты). Возвращает
// текст ошибки для 400 или nullopt, если всё в порядке (тогда out заполнен).
// Тексты — дословно из spec «Проверка параметров фильтра»; порядок проверок:
// category_uuid → exclude_category_uuid → оба вместе → wallet_uuid → kind → q.
// q_values — все значения повторяемого параметра q по порядку; пустые
// отбрасываются (пустое значение равносильно отсутствию).
std::optional<std::string> parse_operations_filter(const std::string& category_uuid,
                                                    const std::string& exclude_category_uuid,
                                                    const std::string& wallet_uuid,
                                                    const std::string& kind,
                                                    const std::vector<std::string>& q_values,
                                                    OperationsFilter& out);

// Один q — форма до search-text-variants (интеграционные тесты фильтров).
inline std::optional<std::string> parse_operations_filter(const std::string& category_uuid,
                                                           const std::string& exclude_category_uuid,
                                                           const std::string& wallet_uuid,
                                                           const std::string& kind, const std::string& q,
                                                           OperationsFilter& out) {
    return parse_operations_filter(category_uuid, exclude_category_uuid, wallet_uuid, kind,
                                   std::vector<std::string>{q}, out);
}

// Прежняя форма — без исключения (тесты фильтров до add-category-month-analytics).
inline std::optional<std::string> parse_operations_filter(const std::string& category_uuid,
                                                           const std::string& wallet_uuid,
                                                           const std::string& kind, const std::string& q,
                                                           OperationsFilter& out) {
    return parse_operations_filter(category_uuid, std::string(), wallet_uuid, kind, q, out);
}

struct OperationsTotal {
    std::string total_eq = "0"; // с kind — неотрицательная сумма, без — доходы минус расходы
    bool incomplete = false;    // у части подходящих операций нет курса — в сумму они не вошли
    long long matched = 0;      // число подходящих операций (прямые переводы тоже)
};

// Итог по фильтру за весь период — f_operations_total, ровно одна строка.
// Те же условия, что у f_operations_for_period (согласованность — тест
// operations_total_matches_all_pages).
OperationsTotal operations_total(db::Database& db, const std::string& ledger_id, const std::string& from,
                                 const std::string& to, const std::string& eq_currency_uuid,
                                 const OperationsFilter& filter);

// limit — сколько строк отдать максимум (вызывающий, reports.cpp, обязан
// проверить диапазон ДО вызова — здесь не проверяется, как и валидность
// from <= to). cursor_date/cursor_uuid — пустые строки ОБА означают "первая
// страница" (вызов без $6/$7 — см. .cpp, почему не NULL-параметр); заданы
// ОБА или НИ ОДНОГО — как cmp_from/cmp_to у expense_structure_for_period,
// вызывающий обязан отклонить смешанный случай ДО вызова.
//
// filter — необязательные фильтры (см. OperationsFilter). Если задан хоть
// один, на том же соединении вторым запросом считается итог
// (operations_total) и result.has_total = true; без фильтров второго
// запроса нет и текст вызова тот же, что до фильтров.
OperationsResult operations_for_period(db::Database& db, const std::string& ledger_id,
                                        const std::string& from, const std::string& to,
                                        const std::string& eq_currency_uuid, int limit,
                                        const std::string& cursor_date, const std::string& cursor_uuid,
                                        const OperationsFilter& filter = {});

// "Кошельки и остатки" (Design/.../Отчёты - веб (офлайн).html, вид 'wal',
// GET /api/reports/wallet-turnover) — обёртка над f_wallet_turnover_for_period:
// оборот КАЖДОГО кошелька леджера за период, и в его собственной валюте, и в
// эквиваленте. Остатки на начало/конец периода (тоже уже в эквиваленте) экран
// берёт из balance_at_dates ниже (одним батч-запросом на несколько дат, а не
// balance_to_date дважды — см. её комментарий и wallet-web/site/app.js::
// refreshWalletBalances).
//
// eq_currency ЗДЕСЬ ЕСТЬ (в отличие от первой версии этой функции) — по
// прямому запросу: сортировка столбцов "Пришло"/"Ушло" на экране обязана
// идти по эквиваленту (сравнимая величина между разными валютами кошельков),
// а не по "своей" сумме, которая для разных валют несравнима в принципе.
// "Своя" сумма (turnover_in/turnover_out) при этом никуда не делась — ей
// по-прежнему подписана каждая строка таблицы.
//
// В отличие от income_for_period/expense_for_period — считается ЛЮБОЕ
// движение по кошельку (включая переводы кошелёк-кошелёк), и возвращаются
// ВСЕ кошельки леджера, а не только тронутые в периоде (zero-fill).
struct WalletTurnoverResult {
    // {account_uuid, account_name, comment, is_archived, currency_uuid,
    // symbol, precision, icon_sha256, turnover_in, turnover_out,
    // turnover_in_eq, turnover_out_eq}, ORDER BY account_name. is_archived —
    // настоящий JSON boolean (не строка "t"/"f", как остальные текстовые поля
    // этой структуры) — тот же приём, что incomplete у ReportResult, только
    // он вычислен на уровне отдельной строки, а не всего результата.
    // turnover_in_eq/turnover_out_eq — та же оговорка "sum() пропускает
    // NULL", что и у total_eq ниже: 0 у строки без единой операции этого
    // направления, но может быть занижен (без явного признака на строке), у
    // строки, где курс НЕ нашёлся хотя бы для одной операции — это и ловит
    // общий incomplete.
    nlohmann::json rows = nlohmann::json::array();
    // true — хотя бы для одной строки ЛЮБОГО из направлений (пришло/ушло) не
    // нашлось курса в eq_currency хотя бы у одной операции — тот же смысл
    // "~", что и в остальных отчётах.
    bool incomplete = false;
};

WalletTurnoverResult wallet_turnover_for_period(db::Database& db, const std::string& ledger_id,
                                                 const std::string& from, const std::string& to,
                                                 const std::string& eq_currency_uuid);

// Баланс каждого кошелька леджера сразу на НЕСКОЛЬКО дат (по прямому запросу
// — "Кошельки и остатки" делали до 14 отдельных GET /api/reports/balance на
// один показ экрана: начало периода, конец периода, плюс 12 точек
// спарклайна; "Обзор" — ещё 12 на линию остатка "Расхода по месяцам". Здесь
// это один запрос, один поход в СУБД). Обёртка над f_balance_to_dates — та же
// формула баланса, что и у balance_to_date (обороты строго ДО даты), просто
// сразу на весь список дат.
struct BalancesAtDatesResult {
    // По одному блоку {date, accounts, total_eq, incomplete} НА КАЖДУЮ дату
    // из dates, переданного в balances_at_dates, В ТОМ ЖЕ ПОРЯДКЕ — включая
    // даты, для которых не нашлось ни одного кошелька с движением (accounts:
    // [], total_eq: "0", incomplete: false), а НЕ пропуская их: вызывающий
    // код обязан получить РОВНО dates.size() блоков, иначе не сможет надёжно
    // сопоставить блок ответа со своим индексом/датой запроса (см. .cpp —
    // date в блоке эхом повторяет строку запроса как есть, не то, что
    // вернул/нормализовал Postgres).
    //
    // accounts — то же самое, что ReportResult.rows у balance_to_date
    // ({account_uuid, account_name, currency_uuid, symbol, precision,
    // balance, balance_eq}).
    nlohmann::json dates = nlohmann::json::array();
};

// dates — уже готовые строки дат (webclient сам решает, "23:59:59" или
// голая дата, см. .cpp у резолва в Postgres timestamp[]); пустой список —
// вызывающий (reports.cpp) обязан отклонить запрос 400 ДО вызова этой
// функции, здесь не проверяется.
BalancesAtDatesResult balances_at_dates(db::Database& db, const std::string& ledger_id,
                                         const std::vector<std::string>& dates,
                                         const std::string& eq_currency_uuid);

// "Обзор"/"Динамика" (по прямому запросу, 2026-09-11 — "новое API, которое
// просит с/по и шагом, а не вызов с миллионом параметров в GET"): та же
// формула баланса, что и balances_at_dates выше, но список дат-границ
// бакетов сервер строит САМ из from/to/step — обёртка над f_balance_series
// (wallet-api/schema/function/f_balance_series.sql), не над f_balance_to_dates.
// Раньше клиент сам считал границы бакетов (день/неделя/месяц) и слал их
// списком в query-параметре dates — при широком периоде с группировкой по
// дням/неделям список разрастался до сотен дат и упирался в лимит длины URI
// на wallet-gateway ("Failed to fetch"). Здесь запрос всегда фиксированного
// размера (from+to+step+eq_currency), независимо от длины периода.
struct BalanceSeriesResult {
    // По одному блоку {bucket_start, bucket_end, accounts, total_eq,
    // incomplete} на каждый бакет окна [from, to], разбитого по step, В
    // ХРОНОЛОГИЧЕСКОМ ПОРЯДКЕ — включая бакеты без единого кошелька с
    // движением (accounts: [], total_eq: "0", incomplete: false); в отличие
    // от balances_at_dates, длина этого списка сервером НЕ переспрашивается
    // у вызывающего — она следует из date_index, который f_balance_series
    // возвращает уже для КАЖДОГО бакета (включая пустые, см. её LEFT JOIN).
    nlohmann::json buckets = nlohmann::json::array();
};

// step — "day"/"week"/"month"/"year"; вызывающий (reports.cpp) обязан
// проверить это ДО вызова (400 на неизвестное значение) — здесь не
// проверяется, как и валидность from &lt;= to.
//
// with_accounts=false (по умолчанию у GET /api/reports/balance-series, по
// прямому запросу 2026-09-22 — "надуло ОЗУ до 300 МБ") — у блока НЕТ поля
// accounts, только bucket_start/bucket_end/total_eq/incomplete, и сумма
// считается в SQL (GROUP BY бакета), а не строкой на каждый счёт. Два года
// по дням с детализацией — ~6.5 МБ JSON и десятки МБ пиковой памяти на
// запрос (PGresult + db::Result + дерево json + dump), при том что веб
// читает только итог. true — прежний ответ с accounts на каждый бакет.
BalanceSeriesResult balance_series(db::Database& db, const std::string& ledger_id,
                                    const std::string& from, const std::string& to,
                                    const std::string& step, const std::string& eq_currency_uuid,
                                    bool with_accounts);

// Админ-таблица (POST /api/reports/admin/users/stats, только Role::Admin, см.
// reports.cpp) — по одной строке на каждого ИЗ ПЕРЕДАННЫХ владельцев леджера (роль
// 'owner' в ledger_member), не на весь ledger_member: MVP держит один
// леджер на пользователя (см. org/docs/TODO_mobile_sync.md, п. 1.3), поэтому
// "владелец" здесь и есть "пользователь", а не отдельная сущность. Если
// когда-нибудь один пользователь станет владельцем нескольких леджеров —
// строк на него станет несколько, вызывающий код (SPA) сейчас на это не
// рассчитан.
//
// Числа — той же текстовой формой, что и весь остальной файл (db::Row
// хранит всё строками, row_to_json просто переносит их в JSON как есть,
// см. её комментарий) — не только "деньги как numeric-строка", как в
// остальных отчётах, а и обычные count(*) заодно, ради одного общего
// правила на файл вместо двух.
struct UserStatsResult {
    // {user_id, ledger_id, wallet_count, operation_count,
    // first_operation_date, last_operation_date, member_count}.
    // first_operation_date/last_operation_date — пустая строка (SQL NULL),
    // если у леджера ни одной живой операции ещё нет — тот же принцип "нет
    // данных — не ошибка", что и у остальных отчётов пустого леджера.
    nlohmann::json users = nlohmann::json::array();
};

// user_ids — уже проверенные на формат UUID (reports.cpp); пустой список — пустой ответ без запроса.
UserStatsResult list_user_stats(db::Database& db, const std::vector<std::string>& user_ids);

// Иконки (GET /api/reports/admin/icons, только Role::Admin, и
// GET /api/reports/icons — свои, см. reports.cpp)
// — по прямому запросу (2026-09-16, "общий блок со всеми общими иконками, и
// выбор пользователя, чтобы посмотреть его личные"). common — общий/
// эталонный набор (icon.ledger_id IS NULL), отдаётся всегда; user — свои
// иконки ЛЕДЖЕРА конкретного пользователя (не самого user_id — см.
// find_ledger_for_user, "один леджер на пользователя" для MVP), пустой
// массив, если ledger_id не передан (админ ещё не выбрал пользователя).
struct IconsResult {
    // {uuid, name, icon_type, sha256} — тот же приём, что icon_sha256 у
    // категорий/валют в остальных отчётах (sha256 — hex content_sha256,
    // саму картинку веб-клиент качает сам через GET /sync/blobs/:sha256),
    // просто здесь сама иконка — строка результата, а не поле внутри отчёта.
    nlohmann::json common = nlohmann::json::array();
    nlohmann::json user = nlohmann::json::array();
};

IconsResult list_icons(db::Database& db, const std::optional<std::string>& ledger_id);

} // namespace reports_repo
