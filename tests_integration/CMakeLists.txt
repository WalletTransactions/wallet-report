# Гейты (WALLET_REPORT_BUILD_INTEGRATION_TESTS / WALLET_REPORT_BUILD_LOADTEST_SEED)
# — в родительском CMakeLists.txt; этот файл собирается, если включён ХОТЯ БЫ
# один из них, и сам решает ниже, какие из двух целей реально строить.
#
# reports_repo.cpp/db.cpp/util/log.cpp — код, который здесь реально
# тестируется (db.cpp сам зовёт applog::info/error на каждый запрос, поэтому
# util/log.cpp обязателен, даже без явного вызова applog::init).
#
# sync_repo.cpp/provisioning.cpp/util/hex.cpp/util/uuid.cpp — НЕ часть
# сервиса wallet_report (см. родительский CMakeLists.txt, почему), нужны
# ИСКЛЮЧИТЕЛЬНО чтобы засеять базу через настоящий push_batch — тот же
# протокол, которым в проде пишет wallet-api, а не сырой SQL (см. fixture.hpp,
# "Почему push_batch, а не SQL"). reports.cpp/session.cpp (HTTP-слой) сюда не
# нужны по той же причине, что и в wallet-api — тянут httplib::Request/
# Response, тестам это не нужно, reports_repo/sync_repo вызываются напрямую.
if(WALLET_REPORT_BUILD_INTEGRATION_TESTS)
    add_executable(wallet_report_integration_tests
        test_main.cpp
        test_reports.cpp

        ${CMAKE_SOURCE_DIR}/src/db.cpp
        ${CMAKE_SOURCE_DIR}/src/reports_repo.cpp
        ${CMAKE_SOURCE_DIR}/src/provisioning.cpp
        ${CMAKE_SOURCE_DIR}/src/sync_repo.cpp
        ${WALLET_COMMON_DIR}/util/uuid.cpp
        ${WALLET_COMMON_DIR}/util/hex.cpp
        ${WALLET_COMMON_DIR}/util/log.cpp
    )

    target_include_directories(wallet_report_integration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
        ${WALLET_COMMON_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_definitions(wallet_report_integration_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
    )
    target_compile_options(wallet_report_integration_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )

    target_link_libraries(wallet_report_integration_tests PRIVATE
        Threads::Threads
        PostgreSQL::PostgreSQL
        # util/uuid.cpp (wallet-common) — v5 через SHA-1 OpenSSL.
        OpenSSL::Crypto
    )

    if(WIN32)
        target_link_libraries(wallet_report_integration_tests PRIVATE ws2_32 crypt32)
    endif()

    # Не add_test()/ctest — та же причина, что в wallet-api: ctest не пробрасывает
    # DATABASE_URL/REPORTS_DATABASE_URL и не поднимает Postgres сам; запуск
    # исключительно руками/скриптом, см. README.md, "Как запустить".
endif()

# wallet_report_loadtest_seed — разовая сеялка данных для нагрузочного
# контура (wallet-report/loadtest/README.md), не тест: те же
# write-.cpp, что и у wallet_report_integration_tests выше (переиспользует
# fixture.hpp::open()/make_reference_icon()), но БЕЗ test_framework.hpp/
# test_main.cpp — обычный main(), не набор TEST().
if(WALLET_REPORT_BUILD_LOADTEST_SEED)
    add_executable(wallet_report_loadtest_seed
        seed_loadtest.cpp

        ${CMAKE_SOURCE_DIR}/src/db.cpp
        ${CMAKE_SOURCE_DIR}/src/provisioning.cpp
        ${CMAKE_SOURCE_DIR}/src/sync_repo.cpp
        ${WALLET_COMMON_DIR}/util/uuid.cpp
        ${WALLET_COMMON_DIR}/util/hex.cpp
        ${WALLET_COMMON_DIR}/util/log.cpp
    )

    target_include_directories(wallet_report_loadtest_seed PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
        ${WALLET_COMMON_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_definitions(wallet_report_loadtest_seed PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
    )
    target_compile_options(wallet_report_loadtest_seed PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )

    target_link_libraries(wallet_report_loadtest_seed PRIVATE
        Threads::Threads
        PostgreSQL::PostgreSQL
        # util/uuid.cpp (wallet-common) — v5 через SHA-1 OpenSSL.
        OpenSSL::Crypto
    )

    if(WIN32)
        target_link_libraries(wallet_report_loadtest_seed PRIVATE ws2_32 crypt32)
    endif()
endif()
