cmake_minimum_required(VERSION 3.14)
project(LogosPilotOwnerPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/metadata.json ${CMAKE_CURRENT_BINARY_DIR}/metadata.json COPYONLY)

find_package(OpenSSL REQUIRED)

# src/owner_client.* and src/pilot_crypto.* are not in this directory's git tree: the flake's
# preConfigure copies them in from pilot-owner/src and pilot-module/src (one home each) right
# before CMake runs. Same pattern as pilot-module: only the module's own sources are listed; the
# builder generates and compiles the universal glue itself.
logos_module(
    NAME pilot_owner
    SOURCES
        src/pilot_owner_impl.h
        src/pilot_owner_impl.cpp
        src/owner_client.h
        src/owner_client.cpp
        src/pilot_crypto.h
        src/pilot_crypto.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/generated_code
)

target_link_libraries(pilot_owner_module_plugin PRIVATE OpenSSL::Crypto)
