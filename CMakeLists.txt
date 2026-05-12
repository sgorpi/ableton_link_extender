cmake_minimum_required(VERSION 3.21 FATAL_ERROR)
project(AbletonLinkExtender
VERSION 0.1.0
LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /wd4100)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wzero-as-null-pointer-constant -Wno-unused-parameter)
endif()

include(CMakeDependentOption)
include(CheckSymbolExists)
include(GNUInstallDirs)

find_package(Boost REQUIRED)

# location of sources:
#add_subdirectory(source)

add_executable(ableton_link_extender
  source/main.cpp
)
target_include_directories(ableton_link_extender PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

include(link/AbletonLinkConfig.cmake)
target_link_libraries(ableton_link_extender Ableton::Link Boost::headers)

if(WIN32)
  target_link_libraries(ableton_link_extender ws2_32 iphlpapi)
endif()

# ===== Unit tests =====
include(link/cmake_include/CatchConfig.cmake)
enable_testing()

set(TEST_SOURCES
  tests/tst_TunnelGateway.cpp
  tests/tst_UdpTunnel.cpp
  tests/tst_Controller.cpp
  tests/tst_TunnelDedup.cpp
)

foreach(TEST_SOURCE ${TEST_SOURCES})
  get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
  add_executable(${TEST_NAME}
    ${TEST_SOURCE}
    link/src/ableton/test/catch/CatchMain.cpp
    link/src/ableton/test/serial_io/SchedulerTree.cpp
  )
  target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(${TEST_NAME} Catch::Catch Ableton::Link Boost::headers)
  target_compile_definitions(${TEST_NAME} PRIVATE -DCATCH_CONFIG_ENABLE_BENCHMARKING=1)
  if(WIN32)
    target_link_libraries(${TEST_NAME} ws2_32 iphlpapi)
  endif()
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
