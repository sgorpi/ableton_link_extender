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
