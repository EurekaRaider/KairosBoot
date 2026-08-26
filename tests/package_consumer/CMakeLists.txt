cmake_minimum_required(VERSION 3.22)
project(KairosBootPackageConsumer LANGUAGES C CXX)

find_package(KairosBoot CONFIG REQUIRED)

add_executable(c_consumer c_consumer.c)
set_target_properties(
  c_consumer
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF)
target_link_libraries(c_consumer PRIVATE KairosBoot::C)

add_executable(cxx_consumer cxx_consumer.cpp)
set_target_properties(
  cxx_consumer
  PROPERTIES CXX_STANDARD 23
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_link_libraries(cxx_consumer PRIVATE KairosBoot::Cxx)
