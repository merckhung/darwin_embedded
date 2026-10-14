cmake_minimum_required(VERSION 3.16)

project(kc-tools C)

set(CMAKE_C_STANDARD 11)

set(common_opts -Wall -Wno-unused-function -Wno-unused-variable)

set(shared_lib
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/linker.c
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/macho_view.c
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/plist.c
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/prelink.c
)

add_executable(kc-builder
  src/kc_builder.c
  src/fileset.c
  ${shared_lib}
)
target_include_directories(kc-builder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(kc-builder PRIVATE ${common_opts})

add_executable(prelink-builder
  src/prelink_builder.c
  ${shared_lib}
)
target_include_directories(prelink-builder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(prelink-builder PRIVATE ${common_opts})

install(TARGETS kc-builder prelink-builder DESTINATION bin)