// GET /api/reports/balance, /api/reports/income, /api/reports/expense
// (org/docs/TODO_mobile_sync.md, Фаза 4; список — там же, 1.6) — тонкий
// HTTP-слой (разбор query-параметров, JSON-ответ) поверх бизнес-логики в
// reports_repo.hpp (та же обвязка sync.hpp/sync_repo.hpp) — SQL-функциям
// `wallet-api/schema/function/f_balance_to_date.sql`/`f_income_for_period.sql`/
// `f_expanse_for_period.sql` и роли `wallet_reader` (см. её README,
// "Отчёты — переиспользовать готовые SQL-функции").
//
// Обычная сессионная аутентификация (session::current_user), БЕЗ
// authenticate_and_provision из sync.cpp — отчёты не несут device_id и
// нужны браузеру (SPA wallet-web) не хуже мобильного клиента. Леджер
// резолвится из уже существующего app_user/ledger_member (см.
// reports_repo::find_ledger_for_user) — В ОТЛИЧИЕ от /sync/*, отчёты
// НИЧЕГО не провизионируют: пользователь, ни разу не синкавший ни с одного
// устройства, просто получит пустой отчёт (200, а не 404 — "нет данных" не
// ошибка), а не будет посажен в новый леджер read-only ролью, которая и не
// смогла бы туда написать.
//
// "Посмотреть как клиент" (Role::Admin) — необязательный query-параметр
// as_user_id у ЛЮБОГО из этих же эндпоинтов подменяет леджер на леджер
// указанного пользователя (см. reports.cpp::resolve_subject_user_id) —
// отдельных /admin/*-копий отчётов нет. Плюс один настоящий admin-only
// эндпоинт без аналога у обычного пользователя — GET /api/reports/admin/users
// (агрегаты по ВСЕМ пользователям для админ-таблицы, см.
// reports_repo::list_user_stats).
#pragma once

#include "app_state.hpp"
#include "httplib.h"

namespace reportsapi {

void register_routes(httplib::Server& svr, AppState& state);

} // namespace reportsapi
