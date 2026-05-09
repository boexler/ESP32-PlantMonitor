# ESP-IDF project for Seeed XIAO ESP32-C6 plant monitor (MQTT + Home Assistant discovery).
cmake_minimum_required(VERSION 3.16)

# Repo root (this file lives under .../firmware).
get_filename_component(PM_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

# Reconfigure when HEAD moves so PROJECT_VER/Git describe stays in sync without editing CMakeLists.
if(EXISTS "${PM_REPO_ROOT}/.git/HEAD")
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                 APPEND
                 PROPERTY
                 CMAKE_CONFIGURE_DEPENDS
                 "${PM_REPO_ROOT}/.git/HEAD")
endif()

# -----------------------------------------------------------------------------
# Firmware version → ESP-IDF PROJECT_VER → PM_FW_VERSION in main/component.
#
# Priority: PM_PROJECT_VER (env, for CI/release) → git describe → short hash → fallback.
# -----------------------------------------------------------------------------

set(PM_RESOLVED_VER "")
if(DEFINED ENV{PM_PROJECT_VER})
    string(STRIP "$ENV{PM_PROJECT_VER}" _pm_env_ver)
    if(NOT _pm_env_ver STREQUAL "")
        set(PM_RESOLVED_VER "${_pm_env_ver}")
    endif()
endif()

if(PM_RESOLVED_VER STREQUAL "")
    find_program(pm_git NAMES git CMAKE_FIND_ROOT_PATH_BOTH)
endif()

set(pm_git_out "")
if(PM_RESOLVED_VER STREQUAL "" AND pm_git)
    execute_process(
        COMMAND "${pm_git}" -C "${PM_REPO_ROOT}" describe --tags --abbrev=12 --dirty
        RESULT_VARIABLE pm_git_rc
        OUTPUT_VARIABLE pm_git_out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(pm_git_rc EQUAL 0 AND NOT pm_git_out STREQUAL "")
        set(PM_RESOLVED_VER "${pm_git_out}")
    endif()
endif()

if(PM_RESOLVED_VER STREQUAL "" AND pm_git)
    execute_process(
        COMMAND "${pm_git}" -C "${PM_REPO_ROOT}" rev-parse --short=12 HEAD
        RESULT_VARIABLE pm_git_rc
        OUTPUT_VARIABLE pm_git_out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(pm_git_rc EQUAL 0 AND NOT pm_git_out STREQUAL "")
        set(PM_RESOLVED_VER "${pm_git_out}")
    endif()
endif()

if(PM_RESOLVED_VER STREQUAL "")
    set(PM_RESOLVED_VER "0.0.0-unknown")
endif()

set(PROJECT_VER "${PM_RESOLVED_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(plant_monitor)
