cmake_minimum_required(VERSION 3.14)
project(LogosPilotPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/metadata.json ${CMAKE_CURRENT_BINARY_DIR}/metadata.json COPYONLY)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

logos_module(
    NAME pilot
    SOURCES
        src/pilot_impl.h
        src/pilot_impl.cpp
        src/pilot_identity.cpp
        src/pilot_owner.cpp
        src/pilot_spending.cpp
        src/pilot_storage.cpp
        src/pilot_messaging.cpp
        src/pilot_meta.cpp
        src/pilot_a2a.cpp
        src/pilot_a2a_inbox.cpp
        src/pilot_llm.h
        src/pilot_llm_anthropic.cpp
        src/pilot_llm_openai.cpp
        src/pilot_llm_factory.cpp
        src/pilot_crypto.h
        src/pilot_crypto.cpp
        src/pilot_skill.h
        src/pilot_plugin_iface.h
        src/pilot_skill_registry.cpp
        src/pilot_builtin_skills.cpp
        generated_code/pilot_qt_glue.h
        generated_code/pilot_dispatch.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/generated_code
)

target_link_libraries(pilot_module_plugin PRIVATE SQLite::SQLite3 OpenSSL::Crypto)

# Optional: build the bundled example third-party skill plugin
# (examples/skill-weather -> libweather_plugin.so). OFF by default so the core
# module build is unaffected; enable with -DPILOT_BUILD_EXAMPLES=ON. The example
# is self-contained and only consumes the two stable public headers
# (pilot_skill.h + pilot_plugin_iface.h), proving a skill can be added at runtime
# without modifying or recompiling the core module.
option(PILOT_BUILD_EXAMPLES "Build bundled example skill plugins" OFF)
if(PILOT_BUILD_EXAMPLES)
    set(PILOT_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH
        "Directory with pilot_skill.h and pilot_plugin_iface.h" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../examples/skill-weather
                     ${CMAKE_CURRENT_BINARY_DIR}/examples/skill-weather)
endif()
