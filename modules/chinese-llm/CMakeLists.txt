set(CHINESE_LLM_SOURCES
    chinese-llm.cpp
    llmbackend.cpp
    llmworker.cpp
)

if (ENABLE_CHINESE_LLM)
add_fcitx5_addon(chinese-llm ${CHINESE_LLM_SOURCES})
target_link_libraries(chinese-llm Fcitx5::Core Fcitx5::Config PkgConfig::Curl Pthread::Pthread nlohmann_json::nlohmann_json)
target_include_directories(chinese-llm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

install(TARGETS chinese-llm DESTINATION "${CMAKE_INSTALL_LIBDIR}/fcitx5")

# This addon ships no translatable strings, so the conf is configured directly
# instead of going through the gettext round trip the other addons use.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/chinese-llm.conf.in.in"
               "${CMAKE_CURRENT_BINARY_DIR}/chinese-llm.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/chinese-llm.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
        COMPONENT config)
endif()
