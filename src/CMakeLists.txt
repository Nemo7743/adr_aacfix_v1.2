cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adr_aacfix C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu11 -fno-builtin -ffreestanding")

add_executable(adr_aacfix main.c)
set_target_properties(adr_aacfix PROPERTIES LINK_FLAGS "-nostdlib")

target_link_libraries(adr_aacfix
  taihen_stub
  SceLibKernel_stub
  SceProcessmgr_stub
  SceKernelModulemgr_stub
  SceSysmem_stub
  SceIofilemgr_stub
)


vita_create_self(adr_aacfix.suprx adr_aacfix CONFIG exports.yml UNSAFE)
