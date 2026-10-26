# Do not install/overwrite fcitx5-chinese-addons.mo. Reuse its reviewed native
# labels in our private catalog and add the LLM settings without modifying it.
find_program(CHINESE_LLM_MSGCAT_EXECUTABLE NAMES msgcat)
if (NOT CHINESE_LLM_MSGCAT_EXECUTABLE)
    message(FATAL_ERROR "gettext msgcat is required for Chinese LLM translations")
endif()
set(_locale_root "${CMAKE_CURRENT_BINARY_DIR}/locale")
set(_catalog "${CMAKE_CURRENT_BINARY_DIR}/zh_CN.po")
set(_mo "${_locale_root}/zh_CN/LC_MESSAGES/chinese-llm.mo")
add_custom_command(OUTPUT "${_mo}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_locale_root}/zh_CN/LC_MESSAGES"
    COMMAND "${CHINESE_LLM_MSGCAT_EXECUTABLE}" --use-first --no-location
        "${CMAKE_CURRENT_SOURCE_DIR}/zh_CN.po" "${PROJECT_SOURCE_DIR}/po/zh_CN.po"
        --output-file "${_catalog}"
    COMMAND "${GETTEXT_MSGFMT_EXECUTABLE}" --check --output-file "${_mo}" "${_catalog}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/zh_CN.po" "${PROJECT_SOURCE_DIR}/po/zh_CN.po"
    VERBATIM)
add_custom_target(chinese-llm-translation ALL DEPENDS "${_mo}")
install(FILES "${_mo}" DESTINATION "${FCITX_INSTALL_LOCALEDIR}/zh_CN/LC_MESSAGES"
    COMPONENT chinese-llm)
set(CHINESE_LLM_TEST_LOCALE_DIR "${_locale_root}" CACHE INTERNAL "Build-tree private translations")
