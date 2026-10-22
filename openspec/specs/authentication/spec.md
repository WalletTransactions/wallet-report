# authentication Specification

## Purpose
Проверка access-токена google-auth без собственного OAuth и общих секретов:
подпись по JWKS, адресат и scope токена, извлечение пользователя и роли.

## Requirements

### Requirement: Источник токена
Сервис SHALL брать токен из заголовка `Authorization: Bearer`, а при его
отсутствии — из cookie `session`.

#### Scenario: Токен в заголовке
- **WHEN** запрос содержит `Authorization: Bearer <jwt>` с валидной подписью
- **THEN** пользователь определяется по этому токену

#### Scenario: Токен в cookie
- **WHEN** заголовка Authorization нет, но есть cookie `session` с валидным JWT
- **THEN** пользователь определяется по cookie

### Requirement: Проверка подписи по JWKS
Сервис SHALL проверять подпись RS256 публичным ключом из
`{AUTH_SERVICE_URL}/.well-known/jwks.json` (с кэшированием в памяти) и срок
действия токена. Из payload извлекаются `sub`, `email`, `name`, `role`.

#### Scenario: Нет токена или подпись неверна
- **WHEN** запрос к `/api/reports/*` без токена, с битой подписью или с истёкшим токеном
- **THEN** сервис отвечает 401 `{"error":"unauthenticated"}`

### Requirement: Адресат и scope токена
Для путей `/api/reports/*` сервис SHALL до хендлеров проверять, что токен с
верной подписью выпущен для audience `wallet-report` (и верным issuer) и
содержит scope `reports:read`. Остальные пути (`/healthz`, `/docs`, ...) scope
не требуют.

#### Scenario: Токен для другого сервиса
- **WHEN** токен с верной подписью имеет чужой `aud` или `iss`
- **THEN** сервис отвечает 401 `invalid_token`, событие `jwt_rejected` пишется в лог

#### Scenario: Нет нужного scope
- **WHEN** токен адресован wallet-report, но не содержит `reports:read`
- **THEN** сервис отвечает 403 `insufficient_scope`

#### Scenario: Токен старого формата без aud
- **WHEN** токен с верной подписью не содержит `aud`
- **THEN** сервис отвечает 401 `invalid_token` («токен старого формата — войдите заново»), событие `jwt_rejected` с `reason: legacy` пишется в лог, метрика `jwt_claims_total{result="legacy"}` растёт

### Requirement: Роль из claim
Роль пользователя SHALL браться из claim `role` проверенного токена; сервис
не хранит роли сам.

#### Scenario: Админская проверка
- **WHEN** хендлер проверяет право администратора
- **THEN** используется только `role == "admin"` из проверенного JWT
