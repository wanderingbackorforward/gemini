cmake_minimum_required(VERSION 3.16)
project(quantd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------- 主库 ----------------
add_library(quantd STATIC
  src/core/config.cpp
  src/market/market_rules.cpp
  src/market/calendar.cpp
  src/data/sim_feed.cpp
  src/portfolio/portfolio.cpp
  src/risk/risk_manager.cpp
  src/gateway/paper_gateway.cpp
  src/strategy/dual_ma.cpp
  src/engine/engine.cpp
  src/engine/order_agent.cpp
)
target_include_directories(quantd PUBLIC src)

find_package(Threads REQUIRED)
target_link_libraries(quantd PUBLIC Threads::Threads)

# ---------------- 主程序 ----------------
add_executable(quantd_bin src/main.cpp)
target_link_libraries(quantd_bin PRIVATE quantd)
set_target_properties(quantd_bin PROPERTIES OUTPUT_NAME quantd)

# ---------------- 单元测试 ----------------
enable_testing()
add_executable(quantd_tests tests/test_core.cpp)
target_link_libraries(quantd_tests PRIVATE quantd)
add_test(NAME core COMMAND quantd_tests)
