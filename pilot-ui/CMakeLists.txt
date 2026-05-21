cmake_minimum_required(VERSION 3.14)
project(PilotUiPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME pilot_ui
    REP_FILE src/pilot_ui.rep
    SOURCES
        src/pilot_ui_interface.h
        src/pilot_ui_plugin.h
        src/pilot_ui_plugin.cpp
)
