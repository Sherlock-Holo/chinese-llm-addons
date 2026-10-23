if (ENABLE_CHINESE_LLM)
    # Installed Fcitx testing metadata may live under share/fcitx5/testing and
    # can still contain an unsubstituted version token. Use deterministic
    # build-tree metadata matching the installed runtime module instead.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/testfrontend.conf"
        "[Addon]\nName=testfrontend\nType=SharedLibrary\nLibrary=libtestfrontend\nCategory=Frontend\n\n[Addon/Dependencies]\n0=core:${REQUIRED_FCITX_VERSION}\n")
    add_custom_target(copy-addon-chinese-llm
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_BINARY_DIR}/modules/chinese-llm/chinese-llm.conf"
            "${CMAKE_CURRENT_BINARY_DIR}/chinese-llm.conf"
        DEPENDS chinese-llm)
endif()

if (ENABLE_CHINESE_LLM_ONLY)
    return()
endif()

add_custom_target(copy-addon DEPENDS
    pinyin-addon.conf.in-fmt
    table.conf.in-fmt
    pinyinhelper.conf.in-fmt
    punctuation.conf.in-fmt
    fullwidth.conf.in-fmt
    chttrans.conf.in-fmt
    )
add_custom_command(TARGET copy-addon COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/im/pinyin/pinyin-addon.conf ${CMAKE_CURRENT_BINARY_DIR}/pinyin.conf)
add_custom_command(TARGET copy-addon COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/im/table/table.conf ${CMAKE_CURRENT_BINARY_DIR}/table.conf)
add_custom_command(TARGET copy-addon COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/modules/punctuation/punctuation.conf ${CMAKE_CURRENT_BINARY_DIR}/punctuation.conf)
add_custom_command(TARGET copy-addon COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/modules/pinyinhelper/pinyinhelper.conf ${CMAKE_CURRENT_BINARY_DIR}/pinyinhelper.conf)
add_custom_command(TARGET copy-addon COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/modules/fullwidth/fullwidth.conf ${CMAKE_CURRENT_BINARY_DIR}/fullwidth.conf)
add_custom_command(TARGET copy-addon COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/modules/chttrans/chttrans.conf ${CMAKE_CURRENT_BINARY_DIR}/chttrans.conf)

if (ENABLE_CLOUDPINYIN)
    add_custom_target(copy-addon-cloudpinyin DEPENDS cloudpinyin.conf.in-fmt)
    add_custom_command(TARGET copy-addon-cloudpinyin COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/modules/cloudpinyin/cloudpinyin.conf ${CMAKE_CURRENT_BINARY_DIR}/cloudpinyin.conf)
endif()
