cmake_minimum_required(VERSION 3.12)
project(externis)

option(EXTERNIS_BUILD_TEST "Build the compiler plugin test" ON)
option(EXTERNIS_INSTALL_TO_PLUGIN_DIR "Install Externis into the GCC plugin directory" ON)

if(NOT EXTERNIS_GCC_PLUGIN_DIR)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=plugin
        OUTPUT_VARIABLE EXTERNIS_GCC_PLUGIN_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

if(NOT EXISTS ${EXTERNIS_GCC_PLUGIN_DIR}/include/gcc-plugin.h)
    message(FATAL_ERROR "gcc-plugin.h not found under ${EXTERNIS_GCC_PLUGIN_DIR}/include")
endif()

message("GCC plugin headers found in " ${EXTERNIS_GCC_PLUGIN_DIR})

add_library(externis SHARED src/insight.cpp src/options.cpp)
target_include_directories(externis PRIVATE ${EXTERNIS_GCC_PLUGIN_DIR}/include)

# Optional, useful for debugging.
find_package(fmt)
if (fmt_FOUND)
    target_link_libraries(externis PRIVATE fmt::fmt)
endif()

set_target_properties(externis PROPERTIES CXX_STANDARD 17)
set_target_properties(externis PROPERTIES COMPILE_FLAGS "-fno-rtti -g -Wall")
set_target_properties(externis PROPERTIES PREFIX "" OUTPUT_NAME "insight")

set(EXTERNIS_PLUGIN_PATH ${CMAKE_BINARY_DIR}/insight.so)

if (EXTERNIS_INSTALL_TO_PLUGIN_DIR)
    install(TARGETS externis DESTINATION ${EXTERNIS_GCC_PLUGIN_DIR})
else()
    install(TARGETS externis)
endif()
