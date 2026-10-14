cmake_minimum_required(VERSION 3.16)

project(iig-tools CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(common_opts -Wall -Wno-unused-function)

add_executable(iig
  src/main.cpp
  src/parse.cpp
  src/codegen.cpp
)
target_include_directories(iig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(iig PRIVATE ${common_opts})

install(TARGETS iig DESTINATION bin)
