cmake_minimum_required(VERSION 3.20)
project(bambu_print_history LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The Bambu Studio history helper requires 64-bit Windows and MSVC.")
endif()

add_executable(bambu-print-history bambu-print-history.cpp)
target_compile_features(bambu-print-history PRIVATE cxx_std_17)
set_property(TARGET bambu-print-history PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
