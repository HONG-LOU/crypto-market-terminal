cmake_minimum_required(VERSION 3.25)

project(crypto_market_terminal VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(crypto-market-terminal
  src/main.cpp
)

target_link_libraries(crypto-market-terminal PRIVATE winhttp)

if(MSVC)
  target_compile_options(crypto-market-terminal PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(crypto-market-terminal PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

install(TARGETS crypto-market-terminal RUNTIME DESTINATION bin)
