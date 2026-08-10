cmake_minimum_required(VERSION 3.14)
project(PilotModuleTests LANGUAGES CXX)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

include(LogosTest)

logos_test(
    NAME pilot_module_tests
    # The impl sources include the per-build generated "logos_sdk.h" (typed dependency
    # clients) since the 2026-08 SDK migration. mkLogosModuleTests generates it into
    # ../generated_code before cmake runs; this hands the dir to logos_test so the header
    # resolves and the generated logos_sdk.cpp is compiled into the test binary. Full path:
    # if(EXISTS) is only well-defined for absolute paths.
    GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../generated_code
    MODULE_SOURCES
        ../src/pilot_impl.cpp
        ../src/pilot_identity.cpp
        ../src/pilot_owner.cpp
        ../src/pilot_spending.cpp
        ../src/pilot_storage.cpp
        ../src/pilot_messaging.cpp
        ../src/pilot_meta.cpp
        ../src/pilot_a2a.cpp
        ../src/pilot_a2a_inbox.cpp
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
        test_a2a_inbox.cpp
        test_a2a_outbound.cpp
        test_agent_card.cpp
        test_boot_order.cpp
)

target_link_libraries(pilot_module_tests PRIVATE SQLite::SQLite3 OpenSSL::Crypto)

# --- Runtime plugin loader: REAL positive-path coverage --------------------
# The loader unit tests in test_skills.cpp cover the safety-critical negative
# paths (off-by-default, missing dir, malformed library skipped) without needing
# a compiled plugin. To also prove the *happy path* — that a genuine third-party
# .so is discovered, its skill REGISTERS and DISPATCHES, and the core was NOT
# recompiled to add it — we build the bundled weather example as an actual
# loadable Qt plugin and hand its directory to the test via PILOT_TEST_PLUGIN_DIR.
#
# Guarded by Qt6 discovery: if Qt6 isn't directly findable here, the macro is
# left undefined and the positive-path test compiles out — the negative-path
# loader tests (the safety contract) always run.
find_package(Qt6 QUIET COMPONENTS Core)
set(PILOT_EXAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../examples/skill-weather)
# Also require the example sources to be PRESENT: the Nix module build sandboxes only
# pilot-module/, so the repo-root examples/ tree is absent there. When it's missing the
# positive-path plugin test compiles out and the negative-path safety tests still run.
if(Qt6_FOUND AND EXISTS ${PILOT_EXAMPLE_DIR}/weather_plugin.cpp)
    set(PILOT_TEST_PLUGIN_OUTDIR ${CMAKE_CURRENT_BINARY_DIR}/test_plugins)

    # A QPluginLoader-loadable plugin is a MODULE library. Built from the SAME
    # example sources a third party would ship — only the two stable public
    # headers (pilot_skill.h + pilot_plugin_iface.h) + QtCore, no core internals.
    add_library(test_weather_plugin MODULE
        ${PILOT_EXAMPLE_DIR}/weather_plugin.cpp
        ${PILOT_EXAMPLE_DIR}/weather_skill.cpp
    )
    set_target_properties(test_weather_plugin PROPERTIES
        AUTOMOC ON                                      # Q_OBJECT / Q_PLUGIN_METADATA
        LIBRARY_OUTPUT_DIRECTORY ${PILOT_TEST_PLUGIN_OUTDIR}
    )
    target_include_directories(test_weather_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src   # pilot_skill.h + pilot_plugin_iface.h
        ${PILOT_EXAMPLE_DIR}                 # weather_skill.h + weather_plugin.json (AUTOMOC FILE)
    )
    target_link_libraries(test_weather_plugin PRIVATE Qt6::Core)

    # Build the plugin before the test, and tell the test where the .so landed.
    add_dependencies(pilot_module_tests test_weather_plugin)
    target_compile_definitions(pilot_module_tests PRIVATE
        PILOT_TEST_PLUGIN_DIR="${PILOT_TEST_PLUGIN_OUTDIR}")
else()
    message(STATUS "[pilot] runtime-plugin positive-path test compiled out "
                   "(Qt6 not directly findable OR examples/ not in this build tree); "
                   "negative-path loader safety tests still run.")
endif()
