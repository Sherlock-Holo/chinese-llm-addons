cmake_minimum_required(VERSION 3.12.0)
project(fcitx5-chinese-addons VERSION 5.1.15)

set(REQUIRED_FCITX_VERSION 5.1.22)
set(REQUIRED_LIBIME_VERSION 1.1.14)

#######################################################################
# Options (the default remains the complete upstream Chinese addons build)
#######################################################################
option(ENABLE_CHINESE_LLM_ONLY "Build only the Chinese LLM addon and its tests" Off)
option(ENABLE_TEST "Build Test" On)
option(ENABLE_COVERAGE "Build the project with gcov support (Need ENABLE_TEST=On)" Off)
set(GCOV_TOOL "gcov" CACHE STRING "Path to gcov tool used by coverage.")
option(ENABLE_OPENCC "Enable OpenCC for chttrans" On)
option(ENABLE_GUI "Enable Configure Gui" On)
option(ENABLE_BROWSER "Enable built-in browser." On)
option(ENABLE_CLOUDPINYIN "Build cloud pinyin addon" On)
option(ENABLE_CHINESE_LLM "Build LLM powered pinyin candidate addon" On)
option(ENABLE_DATA "Build data" On)
option(ENABLE_TOOLS "Build tools" On)
set(CHINESE_LLM_JSON_INCLUDE_DIR "" CACHE PATH "Optional directory containing nlohmann/json.hpp (>= 3.2)")

if (ENABLE_CHINESE_LLM_ONLY)
    if (NOT ENABLE_CHINESE_LLM)
        message(FATAL_ERROR "ENABLE_CHINESE_LLM_ONLY requires ENABLE_CHINESE_LLM=On")
    endif()
    # Use normal variables, not FORCE cache writes: switching back to the
    # upstream build restores the caller's original options.
    set(ENABLE_OPENCC Off)
    set(ENABLE_GUI Off)
    set(ENABLE_BROWSER Off)
    set(ENABLE_CLOUDPINYIN Off)
    set(ENABLE_DATA Off)
    set(ENABLE_TOOLS Off)
else()
    find_package(ECM 1.0.0 REQUIRED)
    set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})
    include(ECMSetupVersion)
    include(ECMUninstallTarget)
endif()
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FeatureSummary)
include(GNUInstallDirs)

find_package(Fcitx5Core ${REQUIRED_FCITX_VERSION} REQUIRED)
find_package(Pthread REQUIRED)
find_package(PkgConfig REQUIRED)
if (NOT ENABLE_CHINESE_LLM_ONLY)
    find_package(Fcitx5Module REQUIRED COMPONENTS Notifications QuickPhrase Spell Clipboard)
    find_package(Fcitx5ModuleLuaAddonLoader)
    find_package(Gettext REQUIRED)
    find_package(LibIMETable ${REQUIRED_LIBIME_VERSION} REQUIRED)
endif()
if (NOT ENABLE_CHINESE_LLM_ONLY OR ENABLE_CHINESE_LLM)
    # The LLM entry now owns its native engine instead of loading pinyin.
    find_package(Boost 1.61 REQUIRED)
    find_package(LibIMEPinyin ${REQUIRED_LIBIME_VERSION} REQUIRED)
endif()
if (ENABLE_CHINESE_LLM)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    set(CHINESE_LLM_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
    if (ENABLE_CHINESE_LLM_ONLY)
        # Stable Fcitx core-module interfaces only; Chinese interfaces are
        # generated locally. No installed PinyinHelper/Punctuation package.
        find_package(Fcitx5Module REQUIRED COMPONENTS Notifications QuickPhrase Spell)
    endif()
endif()

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if (ENABLE_OPENCC)
    find_package(OpenCC 1.0.1 REQUIRED)
endif()
if (ENABLE_OPENCC OR ENABLE_CLOUDPINYIN OR ENABLE_CHINESE_LLM)
    if (CHINESE_LLM_JSON_INCLUDE_DIR)
        # Offline/header-only fallback is explicit; never download dependencies
        # implicitly during configuration or replace a system package silently.
        include(CheckCXXSourceCompiles)
        set(_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
        set(CMAKE_REQUIRED_INCLUDES "${CHINESE_LLM_JSON_INCLUDE_DIR}")
        set(_json_forward_include "")
        if (ENABLE_CLOUDPINYIN)
            set(_json_forward_include "#include <nlohmann/json_fwd.hpp>")
        endif()
        unset(CHINESE_LLM_JSON_VERSION_OK CACHE)
        check_cxx_source_compiles("#include <nlohmann/json.hpp>
${_json_forward_include}
#if NLOHMANN_JSON_VERSION_MAJOR < 3 || (NLOHMANN_JSON_VERSION_MAJOR == 3 && NLOHMANN_JSON_VERSION_MINOR < 2)
#error nlohmann_json 3.2 or newer is required
#endif
int main() { nlohmann::json j; return j.is_null() ? 0 : 1; }" CHINESE_LLM_JSON_VERSION_OK)
        set(CMAKE_REQUIRED_INCLUDES "${_saved_required_includes}")
        unset(_saved_required_includes)
        unset(_json_forward_include)
        if (NOT CHINESE_LLM_JSON_VERSION_OK)
            message(FATAL_ERROR "CHINESE_LLM_JSON_INCLUDE_DIR must contain usable nlohmann/json.hpp >= 3.2 (and json_fwd.hpp when building cloudpinyin)")
        endif()
        add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
        set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${CHINESE_LLM_JSON_INCLUDE_DIR}")
    else()
        find_package(nlohmann_json 3.2 REQUIRED)
    endif()
endif()

if (ENABLE_GUI)
    set(QT_MAJOR_VERSION 6)
    find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Concurrent)
    find_package(Fcitx5Qt${QT_MAJOR_VERSION}WidgetsAddons REQUIRED)
    if (ENABLE_BROWSER)
        find_package(Qt${QT_MAJOR_VERSION}WebEngineWidgets REQUIRED)
    endif()
endif()

if (ENABLE_CLOUDPINYIN OR ENABLE_CHINESE_LLM)
    pkg_check_modules(Curl REQUIRED IMPORTED_TARGET "libcurl")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_definitions(-DFCITX_GETTEXT_DOMAIN=\"fcitx5-chinese-addons\")
add_definitions(-DQT_NO_KEYWORDS)
fcitx5_add_i18n_definition()

if (ENABLE_CHINESE_LLM_ONLY)
    add_subdirectory(modules/chinese-llm)
else()
    add_subdirectory(modules)
    add_subdirectory(im)
    add_subdirectory(po)
    if (ENABLE_TOOLS)
        add_subdirectory(tools)
    endif()
    if (ENABLE_GUI)
        add_subdirectory(gui)
    endif()
    add_subdirectory(icon)

    fcitx5_translate_desktop_file(org.fcitx.Fcitx5.Addon.ChineseAddons.metainfo.xml.in
        org.fcitx.Fcitx5.Addon.ChineseAddons.metainfo.xml XML)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Fcitx5.Addon.ChineseAddons.metainfo.xml"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

if (ENABLE_TEST)
    find_package(Fcitx5ModuleTestFrontend REQUIRED)
    if (NOT ENABLE_CHINESE_LLM_ONLY)
        find_package(Fcitx5ModuleTestIM REQUIRED)
    endif()
    enable_testing()
    add_subdirectory(test)
    if (ENABLE_COVERAGE)
        add_custom_target(coverage
            COMMAND "${CMAKE_CTEST_COMMAND}"
            COMMAND lcov --gcov-tool "${GCOV_TOOL}" --no-external --capture --directory ./ -b "${CMAKE_CURRENT_SOURCE_DIR}" --output-file coverage.info
            COMMAND genhtml coverage.info --output-directory "coverage_pages"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
