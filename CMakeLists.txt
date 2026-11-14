cmake_minimum_required(VERSION 3.16)
project(wallet_report CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Общий код сервисов (util/: jwt, jwks, crypto, log, metrics, ...) — копия
# репозитория wallet-common, обновляется его scripts/vendor.sh; руками не править.
set(WALLET_COMMON_DIR ${CMAKE_SOURCE_DIR}/third_party/wallet-common)

# OpenSSL — только для RSA-SHA256-verify (проверка подписи access-токена по
# публичному ключу с JWKS auth-сервиса, см. src/util/crypto.cpp — никакого
# приватного ключа/секрета тут нет, ровно как в wallet-api/wallet-gateway).
# PostgreSQL — ТА ЖЕ база, что у wallet-api (см. README, "Шареная БД, разные
# роли"), но сервис подключается к ней пулом read-only соединений (см.
# src/db.hpp::Pool). ZLIB — gzip-сжатие ответов /api/reports/* (по прямому
# запросу — "правь все пункты по нагрузке"): cpp-httplib (third_party/httplib.h)
# уже умеет сжимать ответ САМ, автоматически, по одному только заголовку
# запроса Accept-Encoding — но ТОЛЬКО если библиотека собрана с определённым
# CPPHTTPLIB_ZLIB_SUPPORT (см. ниже) и зета слинкована; без этого макроса весь
# код сжатия внутри httplib.h попросту не компилируется (#ifdef), и сервис
# всегда отвечал незапакованным JSON, каким бы большим он ни был. wallet-gateway
# (единственная точка входа для браузера) сам ничего не сжимает и не трогает —
# он уже прозрачно форвардит и Accept-Encoding апстриму, и Content-Encoding
# обратно браузеру байт-в-байт (см. его src/main.cpp, proxy_request) — так что
# включения тут, в реальном источнике ответа, достаточно на весь путь целиком.
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(ZLIB REQUIRED)

# Версия приложения — проставляется на билде, не хардкодится в исходниках
# (см. GET /version, src/docs.cpp, и build_info в GET /metrics, src/util/
# metrics.cpp). Единственный источник самого номера — файл VERSION рядом с
# этим CMakeLists.txt (semver, правится руками при релизе); он же копируется
# в Containerfile (COPY VERSION ./), поэтому одинаково читается что при
# нативной сборке (cmake -B build), что при сборке образа.
#
# Git-хэш — ДОПОЛНИТЕЛЬНО к VERSION, не вместо: отличает два билда с одним и
# тем же номером версии между релизами. У сборки ВНУТРИ контейнера своего
# .git нет (Containerfile копирует только исходники, не весь репозиторий) —
# поэтому сначала смотрим на кэш-переменную GIT_COMMIT: её передаёт
# Containerfile (-DGIT_COMMIT=${GIT_COMMIT}, а туда — ARG GIT_COMMIT,
# который в свою очередь снаружи передаёт `podman build --build-arg
# GIT_COMMIT=$(git rev-parse --short HEAD)`, см. compose.yaml). Нативная
# сборка на рабочей машине эту переменную не задаёт — тогда спрашиваем git
# напрямую, репозиторий уже на месте. Не нашли ни то, ни другое — не падаем,
# версия из VERSION всё равно есть, просто без хэша ("unknown").
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION APP_VERSION_LINES LIMIT_COUNT 1)
list(GET APP_VERSION_LINES 0 APP_VERSION)
string(STRIP "${APP_VERSION}" APP_VERSION)

# Сборка — тег образа (`0.1.1-rc.2`), отдельно от VERSION: по VERSION сверяет
# выкат smoke в wallet-infra deploy.sh. Release передаёт тег через
# --build-arg APP_BUILD (Containerfile → -DAPP_BUILD); сборка без него
# помечается `-dev`, чтобы её не спутать с релизной (GET /version, поле
# build; wallet-gateway openspec/changes/service-versions, design D1/D2).
if(NOT DEFINED APP_BUILD OR "${APP_BUILD}" STREQUAL "")
    set(APP_BUILD "${APP_VERSION}-dev")
endif()

if(NOT DEFINED GIT_COMMIT OR "${GIT_COMMIT}" STREQUAL "")
    find_program(GIT_EXECUTABLE git)
    set(GIT_COMMIT "unknown")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty --abbrev=8
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE GIT_DESCRIBE_RESULT
            ERROR_QUIET
        )
        if(NOT GIT_DESCRIBE_RESULT EQUAL 0 OR "${GIT_COMMIT}" STREQUAL "")
            set(GIT_COMMIT "unknown")
        endif()
    endif()
endif()
message(STATUS "wallet_report version: ${APP_VERSION}, build ${APP_BUILD} (${GIT_COMMIT})")

configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

# sync_repo.cpp/provisioning.cpp/util/hex.cpp/util/uuid.cpp сознательно НЕ
# входят в этот список — это код write-стороны (протокол /sync/*), сюда
# перенесённый ТОЛЬКО ради интеграционных тестов ниже (см.
# tests_integration/CMakeLists.txt и fixture.hpp: там им нужно чем-то
# засеять базу, не переизобретая сеялку через сырой SQL, см. README
# tests_integration, "Почему push_batch, а не SQL"). В сам исполняемый файл
# сервиса они не попадают — wallet_report физически не умеет писать, даже
# теоретически, ни одной строчки кода с INSERT/UPDATE тут нет.
add_executable(wallet_report
    src/main.cpp
    src/session.cpp
    src/access.cpp
    src/db.cpp
    src/reports.cpp
    src/reports_repo.cpp
    src/docs.cpp
    ${WALLET_COMMON_DIR}/util/base64.cpp
    ${WALLET_COMMON_DIR}/util/crypto.cpp
    ${WALLET_COMMON_DIR}/util/jwt.cpp
    ${WALLET_COMMON_DIR}/util/jwt_claims.cpp
    ${WALLET_COMMON_DIR}/util/jwks.cpp
    ${WALLET_COMMON_DIR}/util/dotenv.cpp
    ${WALLET_COMMON_DIR}/util/log.cpp
    ${WALLET_COMMON_DIR}/util/metrics.cpp
    ${WALLET_COMMON_DIR}/util/pg_recover.cpp
)

target_include_directories(wallet_report PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${WALLET_COMMON_DIR}
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(wallet_report PRIVATE
    CPPHTTPLIB_ZLIB_SUPPORT
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
target_compile_options(wallet_report PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(wallet_report PRIVATE
    OpenSSL::Crypto
    Threads::Threads
    PostgreSQL::PostgreSQL
    ZLIB::ZLIB
)

if(WIN32)
    target_link_libraries(wallet_report PRIVATE ws2_32 crypt32)

    add_custom_command(TARGET wallet_report POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/scripts/windows
                $<TARGET_FILE_DIR:wallet_report>
        COMMENT "Копирую scripts/windows/*.bat рядом с wallet_report.exe"
    )

    add_custom_command(TARGET wallet_report POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/openapi.yaml
                $<TARGET_FILE_DIR:wallet_report>/openapi.yaml
        COMMENT "Копирую openapi.yaml рядом с wallet_report.exe (GET /openapi.yaml, /docs)"
    )
    add_custom_command(TARGET wallet_report POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/third_party/swagger-ui
                $<TARGET_FILE_DIR:wallet_report>/vendor/swagger-ui
        COMMENT "Копирую вендоренный Swagger UI рядом с wallet_report.exe (GET /docs)"
    )

    # libpq — динамическая библиотека (libpq.dll), тот же приём, что в
    # CMakeLists.txt wallet-api/google-auth.
    get_filename_component(POSTGRESQL_ROOT_DIR "${PostgreSQL_INCLUDE_DIR}" DIRECTORY)
    set(POSTGRESQL_BIN_DIR "${POSTGRESQL_ROOT_DIR}/bin")
    foreach(_pg_dll libpq.dll libssl-3-x64.dll libcrypto-3-x64.dll libintl-9.dll libiconv-2.dll libwinpthread-1.dll)
        if(EXISTS "${POSTGRESQL_BIN_DIR}/${_pg_dll}")
            add_custom_command(TARGET wallet_report POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${POSTGRESQL_BIN_DIR}/${_pg_dll}"
                        $<TARGET_FILE_DIR:wallet_report>
                COMMENT "Копирую ${_pg_dll} рядом с wallet_report.exe"
            )
        endif()
    endforeach()
else()
    add_custom_command(TARGET wallet_report POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/scripts/linux
                $<TARGET_FILE_DIR:wallet_report>
        COMMENT "Копирую scripts/linux/*.sh рядом с wallet_report"
    )
    add_custom_command(TARGET wallet_report POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/openapi.yaml
                $<TARGET_FILE_DIR:wallet_report>/openapi.yaml
        COMMENT "Копирую openapi.yaml рядом с wallet_report (GET /openapi.yaml, /docs)"
    )
    add_custom_command(TARGET wallet_report POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/third_party/swagger-ui
                $<TARGET_FILE_DIR:wallet_report>/vendor/swagger-ui
        COMMENT "Копирую вендоренный Swagger UI рядом с wallet_report (GET /docs)"
    )
endif()

# Интеграционные тесты (wallet_report_integration_tests) — OFF по умолчанию,
# нужен живой Postgres со схемой wallet-api (REPORTS_DATABASE_URL и, только
# для сеялки теста, DATABASE_URL — см. tests_integration/README.md).
option(WALLET_REPORT_BUILD_INTEGRATION_TESTS
       "Собирать интеграционные тесты wallet_report_integration_tests (нужен живой Postgres)" OFF)

# Разовая утилита wallet_report_loadtest_seed (wallet-report/loadtest/) —
# заводит синтетических пользователей с реалистичным объёмом данных для
# нагрузочного тестирования, тем же push_batch(), что и интеграционные тесты
# (см. tests_integration/README.md, "Почему push_batch, а не SQL"), но пишет
# НЕ в одноразовую тестовую базу, а в ту, что укажет DATABASE_URL (обычно —
# dev-база уже поднятого `podman compose` стека, см. loadtest/README.md).
# Отдельный флаг, не переиспользует WALLET_REPORT_BUILD_INTEGRATION_TESTS —
# это не тест, гонять его незачем при обычном прогоне тестов.
option(WALLET_REPORT_BUILD_LOADTEST_SEED
       "Собирать утилиту wallet_report_loadtest_seed для нагрузочного контура (нужен живой Postgres)" OFF)

if(WALLET_REPORT_BUILD_INTEGRATION_TESTS OR WALLET_REPORT_BUILD_LOADTEST_SEED)
    if(WALLET_REPORT_BUILD_INTEGRATION_TESTS)
        enable_testing()
    endif()
    add_subdirectory(tests_integration)
endif()
