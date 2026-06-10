cmake_minimum_required(VERSION 3.14)
project(PilotModuleTests LANGUAGES CXX)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

include(LogosTest)

logos_test(
    NAME pilot_module_tests
    MODULE_SOURCES
        ../src/pilot_impl.cpp
        ../src/pilot_identity.cpp
        ../src/pilot_owner.cpp
        ../src/pilot_spending.cpp
        ../src/pilot_storage.cpp
        ../src/pilot_messaging.cpp
        ../src/pilot_meta.cpp
        ../src/pilot_a2a.cpp
        ../src/pilot_llm_anthropic.cpp
        ../src/pilot_llm_openai.cpp
        ../src/pilot_llm_factory.cpp
        ../src/pilot_crypto.cpp
        ../src/pilot_skill_registry.cpp
        ../src/pilot_builtin_skills.cpp
    TEST_SOURCES
        main.cpp
        test_pilot.cpp
        test_crypto.cpp
        test_skills.cpp
        test_llm.cpp
        test_spending.cpp
)

target_link_libraries(pilot_module_tests PRIVATE SQLite::SQLite3 OpenSSL::Crypto)
