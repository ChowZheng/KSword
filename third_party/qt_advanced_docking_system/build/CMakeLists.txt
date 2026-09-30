cmake_minimum_required(VERSION 3.16)
project(KSwordQtADS LANGUAGES CXX)

# Build the pinned upstream checkout with KSword's existing runtime filenames.
set(KSWORD_ADS_SOURCE_DIR "" CACHE PATH "Qt ADS v5.1.1 source checkout")
if(NOT EXISTS "${KSWORD_ADS_SOURCE_DIR}/src/CMakeLists.txt")
    message(FATAL_ERROR "Set KSWORD_ADS_SOURCE_DIR to the Qt ADS v5.1.1 checkout")
endif()
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(QT_VERSION_MAJOR 6)
if(MSVC)
    add_compile_options(/MP)
endif()
add_subdirectory("${KSWORD_ADS_SOURCE_DIR}" upstream)
set_target_properties(qtadvanceddocking-qt6 PROPERTIES
    OUTPUT_NAME qtadvanceddocking
    DEBUG_POSTFIX d)
