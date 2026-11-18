cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()

project(logonomic_cli_openrig VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The OpenRig add-on package targets Linux x86_64 and macOS arm64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must provide the Logonomic CLI add-on SDK.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

function(openrig_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endfunction()

add_library(openrig_provider STATIC src/openrig_launcher.cpp)
target_include_directories(openrig_provider PUBLIC src)
target_link_libraries(openrig_provider PUBLIC logonomic_cli_addon_sdk::sdk)
openrig_target(openrig_provider)

add_library(openrig_member STATIC src/openrig_member.cpp)
target_include_directories(openrig_member PUBLIC src)
target_link_libraries(openrig_member PUBLIC logonomic_cli_addon_sdk::sdk)
openrig_target(openrig_member)

add_executable(openrig_adapter src/adapter.cpp)
target_link_libraries(openrig_adapter PRIVATE openrig_provider)
openrig_target(openrig_adapter)

add_executable(openrig_prepare_member src/openrig_prepare_member.cpp)
target_link_libraries(openrig_prepare_member PRIVATE openrig_member)
openrig_target(openrig_prepare_member)

logonomic_cli_addon_package_platform(OPENRIG_PLATFORM)
set(OPENRIG_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(openrig_package logonomic.cli.openrig OPENRIG_PACKAGE_DIRECTORY
    ADAPTER openrig_adapter HELPERS openrig_prepare_member)

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
