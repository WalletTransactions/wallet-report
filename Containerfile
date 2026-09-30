# syntax=docker/dockerfile:1
#
# Multi-stage build wallet-report. Как и wallet-api — нужен libpq (шареная
# Postgres-база, см. README "Шареная БД, разные роли"), плюс OpenSSL для
# RSA-SHA256-verify (проверка подписи access-токена по JWKS auth-сервиса —
# без приватного ключа/секрета). curl в рантайме — только для healthcheck'а
# в compose.yaml (GET /healthz), сам образ его не содержит.

FROM debian:bookworm-slim AS build

# GIT_COMMIT — короткий хэш коммита (git rev-parse --short HEAD/git describe
# на хосте, где ЕСТЬ .git — у этой стадии своего нет, .git в контекст сборки
# не входит), передаётся снаружи через `podman build --build-arg
# GIT_COMMIT=...` (см. compose.yaml). Не передали — CMakeLists.txt сам
# попробует спросить git и, если не найдёт, поставит "unknown" (версия из
# VERSION при этом всё равно на месте, см. CMakeLists.txt, "Версия
# приложения").
ARG GIT_COMMIT=unknown

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libssl-dev \
        libpq-dev \
        zlib1g-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY VERSION ./
COPY openapi.yaml ./
COPY src/ src/
COPY third_party/ third_party/
COPY scripts/ scripts/
# ^ нужен: CMakeLists.txt в Linux-ветке копирует scripts/linux/*.sh рядом с
#   бинарником через POST_BUILD add_custom_command (copy_directory), без
#   EXISTS-проверки — без этой папки в контексте сборка упадёт. openapi.yaml
#   рядом по той же причине (см. CMakeLists.txt) — тот же приём, что в
#   Containerfile wallet-api.

# tests_integration/ сюда нарочно НЕ копируется (образу рантайма
# интеграционные тесты не нужны, а сеялка теста тянет write-код sync_repo/
# provisioning — см. её README) — WALLET_REPORT_BUILD_INTEGRATION_TESTS уже
# OFF по умолчанию (см. CMakeLists.txt), поэтому configure и без явного флага
# не пытается зайти в эту директорию.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DGIT_COMMIT="${GIT_COMMIT}" \
    && cmake --build build --config Release -j"$(nproc)"

FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libssl3 \
        libpq5 \
        zlib1g \
        curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /app --shell /usr/sbin/nologin appuser

WORKDIR /app
COPY --from=build /src/build/wallet_report /app/wallet_report
# openapi.yaml + вендоренный Swagger UI — читаются с диска по относительному
# пути при каждом GET /openapi.yaml, /docs (см. src/docs.cpp), не встроены в
# бинарник — копируются прямо из контекста сборки, без прохода через стадию
# build (это чистая статика, компилировать нечего).
COPY openapi.yaml /app/openapi.yaml
COPY third_party/swagger-ui/ /app/vendor/swagger-ui/

# LOG_DIR не задан -> util/log.cpp пишет в stdout/stderr на Linux (см. README
# google-auth, раздел "Логирование" — тут то же самое).
USER appuser
EXPOSE 8084

ENTRYPOINT ["/app/wallet_report"]
