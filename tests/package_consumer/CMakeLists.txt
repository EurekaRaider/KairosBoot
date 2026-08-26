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
target_compile_features(cxx_consumer PRIVATE cxx_std_23)
target_link_libraries(cxx_consumer PRIVATE KairosBoot::Cxx)
