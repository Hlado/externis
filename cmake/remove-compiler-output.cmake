# remove_objs.cmake - a small build-time script
# This file will be executed at build time, not configure time

# Collect all .o files in the binary directory
file(GLOB_RECURSE GENERATED_FILES "${CMAKE_BINARY_DIR}/*.o")

# Add trace.collapsed to the same list
list(APPEND GENERATED_FILES "${CMAKE_BINARY_DIR}/trace.collapsed")

# Loop through and remove them
foreach(f ${GENERATED_FILES})
    message(STATUS "Removing ${f}")
    file(REMOVE ${f})
endforeach()