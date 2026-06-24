# Example third-party Pilot skill plugin — self-contained.
# ---------------------------------------------------------------------------
# Builds libweather_plugin.so, a runtime-loadable Pilot plugin. A third party
# can build this WITHOUT the rest of the core module: it depends only on the
# two stable public headers (pilot_skill.h + pilot_plugin_iface.h) and QtCore.
#
# Standalone build (outside the module tree):
#   cmake -S examples/skill-weather -B build-weather \
#         -DPILOT_SRC_DIR=/path/to/pilot-module/src
#   cmake --build build-weather
#   cp build-weather/*weather_plugin.so ~/.pilot/plugins/   # PILOT_ENABLE_PLUGINS=1
#
# It is also wired as an OPTIONAL target from pilot-module/CMakeLists.txt
# (-DPILOT_BUILD_EXAMPLES=ON), in which case PILOT_SRC_DIR is set for you.
cmake_minimum_required(VERSION 3.16)
project(pilot_weather_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Location of the two stable public headers (pilot_skill.h, pilot_plugin_iface.h).
# When built from the module tree the parent passes this in; otherwise default to
# the in-repo location relative to this file.
if(NOT DEFINED PILOT_SRC_DIR)
    set(PILOT_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../pilot-module/src"
        CACHE PATH "Directory containing pilot_skill.h and pilot_plugin_iface.h")
endif()

# Q_OBJECT / Q_PLUGIN_METADATA require moc.
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core)

# A Qt plugin loaded by QPluginLoader is a MODULE library.
add_library(weather_plugin MODULE
    weather_plugin.cpp
    weather_skill.cpp
)

target_include_directories(weather_plugin PRIVATE
    ${PILOT_SRC_DIR}            # pilot_skill.h + pilot_plugin_iface.h
    ${CMAKE_CURRENT_SOURCE_DIR} # weather_skill.h + weather_plugin.json (AUTOMOC FILE)
)

# Only QtCore — the plugin contract pulls in nothing from the core module.
target_link_libraries(weather_plugin PRIVATE Qt6::Core)
