# ESP32-P4 Launcher (factory app).
# Board selection:  idf.py -DLX_BOARD=<board_id> build   (default: guition_p4_43)
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED LX_BOARD)
    set(LX_BOARD "guition_p4_43")
endif()
set(LX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
if(NOT EXISTS "${LX_ROOT}/boards/${LX_BOARD}/board/CMakeLists.txt")
    message(FATAL_ERROR "Unknown board '${LX_BOARD}' (no boards/${LX_BOARD}/board)")
endif()

set(EXTRA_COMPONENT_DIRS "${LX_ROOT}/components" "${LX_ROOT}/boards/${LX_BOARD}")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${LX_ROOT}/boards/${LX_BOARD}/sdkconfig.board")
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${LX_BOARD}")
# The component manager's "newer versions?" check solves each manifest alone, so it sees
# esp_hosted releases beyond the board's pin; newer ones have dependencies under Kconfig
# options this project doesn't have (ESP_HOSTED_HOST_USB_*), and the configure step fails.
# Versions are pinned per board anyway (boards/<id>/board/idf_component.yml).
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} 0)
# The image's version is what the SD update compares
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LX_ROOT}/components/lx_common/include/lx_common.h")
file(STRINGS "${LX_ROOT}/components/lx_common/include/lx_common.h" LX_VER_LINE REGEX "LX_LAUNCHER_VERSION")
string(REGEX MATCH "\"([^\"]+)\"" _ "${LX_VER_LINE}")
set(PROJECT_VER "${CMAKE_MATCH_1}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Releases are built and tested with this ESP-IDF; the components are pinned to match
# (boards/<id>/board/idf_component.yml). Another ESP-IDF still builds, with this warning.
set(LX_IDF_TESTED "5.5.6")
if(NOT IDF_VERSION_MAJOR STREQUAL "" AND
   NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL LX_IDF_TESTED)
    message(WARNING "ESP-IDF ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}: the launcher "
                    "is built and tested with ESP-IDF v${LX_IDF_TESTED}. If something fails, try that version.")
endif()
# The project name is in the image: an update refuses an image whose name differs,
# so each board only ever installs its own launcher (the first board keeps the old name)
if(LX_BOARD STREQUAL "guition_p4_43")
    project(launcher)
else()
    project(launcher_${LX_BOARD})
endif()
