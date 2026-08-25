cmake_minimum_required(VERSION 3.28)
project(itch_bench CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sender and receiver are two processes on this one host, so nothing has to run
# anywhere else and the build can use everything the CPU offers.
set(ITCH_ARCH "native" CACHE STRING "value passed to -march")
add_compile_options(-O3 -march=${ITCH_ARCH} -Wall -Wextra)

add_library(itch INTERFACE)
target_include_directories(itch INTERFACE ${CMAKE_SOURCE_DIR}/src)

foreach(tool itch_scan book_ref make_packets make_prev_close book_check frame_check)
  add_executable(${tool} src/tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE itch)
endforeach()

# The tools that drive the card. Everything above builds without ef_vi, so a
# machine that only has the data can still run the checks.
find_library(CIUL_LIBRARY NAMES ciul1 ciul)
find_path(CIUL_INCLUDE etherfabric/ef_vi.h)
if(CIUL_LIBRARY AND CIUL_INCLUDE)
  foreach(tool sender wire_check tcp_check)
    add_executable(${tool} src/tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE itch ${CIUL_LIBRARY})
    target_include_directories(${tool} PRIVATE ${CIUL_INCLUDE})
  endforeach()

  # The order path also needs Onload, for the connection whose one packet we
  # push out ourselves.
  find_library(ONLOAD_EXT_LIBRARY NAMES onload_ext)
  find_path(ONLOAD_EXT_INCLUDE onload/extensions.h)
  if(ONLOAD_EXT_LIBRARY AND ONLOAD_EXT_INCLUDE)
    foreach(tool ds_check trader)
      add_executable(${tool} src/tools/${tool}.cpp)
      target_link_libraries(${tool} PRIVATE itch ${CIUL_LIBRARY} ${ONLOAD_EXT_LIBRARY})
      target_include_directories(${tool} PRIVATE ${CIUL_INCLUDE} ${ONLOAD_EXT_INCLUDE})
    endforeach()
  else()
    message(WARNING "onload extensions not found, ds_check is not built")
  endif()
else()
  message(WARNING "ef_vi not found, the tools that drive the card are not built")
endif()

# One test binary per module. GoogleTest is the only thing this repository
# links that it does not ship, and only the tests link it, so the library and
# the tools still build on a machine that does not have it.
find_package(GTest)
if(GTest_FOUND)
  enable_testing()
  include(GoogleTest)
  foreach(mod itch sha256 seq_reader book pool_map price_levels hist samples imbalance ring ouch window mold pack pace eth mintcp huge)
    add_executable(test_${mod} tests/test_${mod}.cpp)
    target_link_libraries(test_${mod} PRIVATE itch GTest::gtest_main)
    gtest_discover_tests(test_${mod})
  endforeach()
else()
  message(WARNING "GoogleTest not found, tests are not built")
endif()
