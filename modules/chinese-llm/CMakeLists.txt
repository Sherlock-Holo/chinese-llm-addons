set(CHINESE_LLM_SOURCES
    chinese-llm.cpp
    llmbackend.cpp
    llmworker.cpp
    llmhistory.cpp
)

if (ENABLE_CHINESE_LLM)
# A private domain keeps installation and runtime binding independent of the
# distribution's Chinese addons. This directory setting also covers native.
remove_definitions(-DFCITX_GETTEXT_DOMAIN=\"fcitx5-chinese-addons\")
add_definitions(-DFCITX_GETTEXT_DOMAIN=\"chinese-llm\")
add_subdirectory(po)
add_subdirectory(native)
add_fcitx5_addon(chinese-llm ${CHINESE_LLM_SOURCES} $<TARGET_OBJECTS:chinese-llm-native-pinyin>)
target_link_libraries(chinese-llm Fcitx5::Core Fcitx5::Config PkgConfig::Curl Pthread::Pthread
    nlohmann_json::nlohmann_json LibIME::Pinyin Fcitx5::Module::Notifications
    Fcitx5::Module::QuickPhrase Fcitx5::Module::Spell)
target_include_directories(chinese-llm BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    ${CHINESE_LLM_NATIVE_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(chinese-llm chinese-llm-translation chinese-llm-native-data chinese-llm-punctuation
    chinese-llm-fullwidth chinese-llm-chttrans chinese-llm-pinyinhelper)

install(TARGETS chinese-llm DESTINATION "${FCITX_INSTALL_ADDONDIR}"
        COMPONENT chinese-llm)

# Metadata carries its own localized entries; configure version/type directly
# without using the upstream addon translation/install component.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/chinese-llm.conf.in.in"
               "${CMAKE_CURRENT_BINARY_DIR}/chinese-llm.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/chinese-llm.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
        COMPONENT chinese-llm)

# A new basename means a new input method identity; never install or replace
# the distribution's inputmethod/pinyin.conf or its pinyin addon library.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/llm-pinyin.conf"
               "${CMAKE_CURRENT_BINARY_DIR}/llm-pinyin.conf" COPYONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/llm-pinyin.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod"
        COMPONENT chinese-llm)
endif()
