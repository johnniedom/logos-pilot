cmake_minimum_required(VERSION 3.16)
project(pilot-owner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The agent's crypto, compiled in (not copied): pilot-module/src/pilot_crypto.cpp. The flake
# passes the directory; a plain cmake build defaults to the sibling checkout.
set(PILOT_CRYPTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pilot-module/src" CACHE PATH
    "Directory holding pilot_crypto.h / pilot_crypto.cpp")

find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)

add_executable(pilot-owner
    src/main.cpp
    ${PILOT_CRYPTO_DIR}/pilot_crypto.cpp
)
target_include_directories(pilot-owner PRIVATE ${PILOT_CRYPTO_DIR})
target_link_libraries(pilot-owner PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)

install(TARGETS pilot-owner RUNTIME DESTINATION bin)
