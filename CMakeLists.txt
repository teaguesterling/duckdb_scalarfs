cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME scalarfs)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/scalarfs_extension.cpp
    src/data_uri_filesystem.cpp
    src/memory_file_handle.cpp
    src/variable_filesystem.cpp
    src/pathvariable_filesystem.cpp
    src/variable_copy_function.cpp
    src/scalarfs_functions.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
