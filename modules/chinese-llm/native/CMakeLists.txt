# Audited build-tree source reuse, not a dependency on the official pinyin addon.
set(CHINESE_LLM_NATIVE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}")
set(CHINESE_LLM_NATIVE_BINARY "${CMAKE_CURRENT_BINARY_DIR}")
set(_pinyin_files pinyin.cpp pinyin.h pinyincandidate.cpp pinyincandidate.h
    customphrase.cpp customphrase.h symboldictionary.cpp symboldictionary.h
    workerthread.cpp workerthread.h)
set(_punctuation_files punctuation.cpp punctuation.h punctuation_public.h)
set(_fullwidth_files fullwidth.cpp fullwidth.h)
set(_chttrans_files chttrans.cpp chttrans.h chttrans-native.cpp chttrans-native.h)
set(_pinyinhelper_files pinyinhelper.cpp pinyinhelper.h pinyinhelper_public.h
    pinyinlookup.cpp pinyinlookup.h stroke.cpp stroke.h)
set(_reviewed_files)
foreach(_file IN LISTS _pinyin_files)
    list(APPEND _reviewed_files "${PROJECT_SOURCE_DIR}/im/pinyin/${_file}")
endforeach()
foreach(_module punctuation fullwidth chttrans pinyinhelper)
    foreach(_file IN LISTS _${_module}_files)
        list(APPEND _reviewed_files "${PROJECT_SOURCE_DIR}/modules/${_module}/${_file}")
    endforeach()
endforeach()
list(APPEND _reviewed_files
    "${PROJECT_SOURCE_DIR}/modules/cloudpinyin/cloudpinyin_public.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/reviewed-sources.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_reviewed_files})

execute_process(
    COMMAND "${CHINESE_LLM_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_CURRENT_BINARY_DIR}"
    RESULT_VARIABLE _generate_status OUTPUT_VARIABLE _generate_output
    ERROR_VARIABLE _generate_error)
if (NOT _generate_status EQUAL 0)
    message(FATAL_ERROR "Private pinyin generation refused: ${_generate_output}${_generate_error}")
endif()
message(STATUS "${_generate_output}")

# The original helpers' public signatures are compile-time interfaces only.
# Never export/install them or depend on installed official Chinese module headers.
set(_native_include_dirs
    "${CMAKE_CURRENT_BINARY_DIR}/pinyin"
    "${CMAKE_CURRENT_BINARY_DIR}/modules/punctuation"
    "${CMAKE_CURRENT_BINARY_DIR}/modules/pinyinhelper"
    "${PROJECT_BINARY_DIR}")

# LibIME stores language models in a libdir on many distributions, unlike
# sc/extb dictionaries in its datadir. Discover the installed model explicitly;
# never use the resolver's global user-search fallback.
get_filename_component(_libime_library_dir "${LibIMECore_DIR}/../.." ABSOLUTE)
find_file(CHINESE_LLM_SYSTEM_LANGUAGE_MODEL NAMES zh_CN.lm
    HINTS "${_libime_library_dir}/libime" "${LIBIME_INSTALL_PKGDATADIR}"
    NO_DEFAULT_PATH DOC "Absolute installed readonly LibIME Chinese language model")
if (NOT CHINESE_LLM_SYSTEM_LANGUAGE_MODEL OR NOT IS_ABSOLUTE "${CHINESE_LLM_SYSTEM_LANGUAGE_MODEL}")
    message(FATAL_ERROR "Set CHINESE_LLM_SYSTEM_LANGUAGE_MODEL to the installed readonly zh_CN.lm")
endif()

set(_pinyin_sources)
foreach(_file pinyin.cpp customphrase.cpp symboldictionary.cpp workerthread.cpp pinyincandidate.cpp)
    list(APPEND _pinyin_sources "${CMAKE_CURRENT_BINARY_DIR}/pinyin/${_file}")
endforeach()
add_library(chinese-llm-native-pinyin OBJECT ${_pinyin_sources})
set_target_properties(chinese-llm-native-pinyin PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(chinese-llm-native-pinyin PRIVATE
    CHINESE_LLM_SYSTEM_LANGUAGE_MODEL="${CHINESE_LLM_SYSTEM_LANGUAGE_MODEL}")
target_include_directories(chinese-llm-native-pinyin BEFORE PRIVATE ${_native_include_dirs} ${Boost_INCLUDE_DIRS})
target_link_libraries(chinese-llm-native-pinyin Fcitx5::Core Fcitx5::Config
    LibIME::Pinyin Fcitx5::Module::Notifications Fcitx5::Module::QuickPhrase
    Fcitx5::Module::Spell Pthread::Pthread)

# Private conversion backend is intentionally native-only, independent of the
# root/upstream OpenCC option and all OpenCC user profile/search paths.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/modules/chttrans/config.h"
    "#pragma once\n/* Private Chttrans deliberately uses the native backend only. */\n")
foreach(_module punctuation fullwidth chttrans pinyinhelper)
    set(_helper_sources "${CMAKE_CURRENT_BINARY_DIR}/modules/${_module}/${_module}.cpp")
    if (_module STREQUAL chttrans)
        list(APPEND _helper_sources "${CMAKE_CURRENT_BINARY_DIR}/modules/chttrans/chttrans-native.cpp")
    elseif (_module STREQUAL pinyinhelper)
        list(APPEND _helper_sources
            "${CMAKE_CURRENT_BINARY_DIR}/modules/pinyinhelper/pinyinlookup.cpp"
            "${CMAKE_CURRENT_BINARY_DIR}/modules/pinyinhelper/stroke.cpp")
    endif()
    add_fcitx5_addon(chinese-llm-${_module} ${_helper_sources})
    target_include_directories(chinese-llm-${_module} BEFORE PRIVATE ${_native_include_dirs})
    target_link_libraries(chinese-llm-${_module} Fcitx5::Core Fcitx5::Config
        Fcitx5::Module::Notifications Pthread::Pthread)
    if (_module STREQUAL pinyinhelper)
        target_link_libraries(chinese-llm-${_module} LibIME::Core)
    endif()
    install(TARGETS chinese-llm-${_module} DESTINATION "${FCITX_INSTALL_ADDONDIR}"
        COMPONENT chinese-llm)
    if (_module STREQUAL pinyinhelper)
        set(_configurable False)
    else()
        set(_configurable True)
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/chinese-llm-${_module}.conf"
        "[Addon]\nName=LLM Pinyin private ${_module}\nCategory=Module\nVersion=${PROJECT_VERSION}\nLibrary=libchinese-llm-${_module}\nType=${FCITX_ADDON_TYPE}\nOnDemand=True\nEnabled=True\nConfigurable=${_configurable}\n\n[Addon/Dependencies]\n0=core:${REQUIRED_FCITX_VERSION}\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/chinese-llm-${_module}.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon" COMPONENT chinese-llm)
endforeach()

# Own readonly data is installed and mirrored into the build tree for smoke.
# No source downloads, tar caches, original user data or official addon needed.
set(_data_root "${CMAKE_CURRENT_BINARY_DIR}/data/chinese-llm")
file(MAKE_DIRECTORY "${_data_root}" "${_data_root}/punctuation"
    "${_data_root}/chttrans" "${_data_root}/pinyinhelper")
foreach(_file symbols sp.dat.example)
    configure_file("${PROJECT_SOURCE_DIR}/im/pinyin/${_file}" "${_data_root}/${_file}" COPYONLY)
    install(FILES "${_data_root}/${_file}" DESTINATION "${FCITX_INSTALL_PKGDATADIR}/chinese-llm"
        COMPONENT chinese-llm)
endforeach()
foreach(_language zh_CN zh_HK zh_TW)
    configure_file("${PROJECT_SOURCE_DIR}/modules/punctuation/punc.mb.${_language}"
        "${_data_root}/punctuation/punc.mb.${_language}" COPYONLY)
    install(FILES "${_data_root}/punctuation/punc.mb.${_language}"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/chinese-llm/punctuation" COMPONENT chinese-llm)
endforeach()
configure_file("${PROJECT_SOURCE_DIR}/modules/chttrans/gbks2t.tab"
    "${_data_root}/chttrans/gbks2t.tab" COPYONLY)
install(FILES "${_data_root}/chttrans/gbks2t.tab"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/chinese-llm/chttrans" COMPONENT chinese-llm)
foreach(_file py_stroke.mb py_table.mb)
    if (_file STREQUAL py_stroke.mb)
        set(_expected_sha 9b526eab3ef2679d63155c8304fc4b11f701c2bf5db7946e7d3e940f54f3aaa9)
    else()
        set(_expected_sha 19b3e593e5a211fe548f39c894d9a2e2e07a08e3edee0083087b1471e5ba026b)
    endif()
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/data/${_file}" _actual_sha)
    if (NOT _actual_sha STREQUAL _expected_sha)
        message(FATAL_ERROR "Unreviewed private lookup data: ${_file}")
    endif()
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/data/${_file}"
        "${_data_root}/pinyinhelper/${_file}" COPYONLY)
    install(FILES "${_data_root}/pinyinhelper/${_file}"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/chinese-llm/pinyinhelper" COMPONENT chinese-llm)
endforeach()
add_custom_command(OUTPUT "${_data_root}/chaizi.dict"
    COMMAND LibIME::pinyindict "${PROJECT_SOURCE_DIR}/im/pinyin/chaizi.txt" "${_data_root}/chaizi.dict"
    DEPENDS "${PROJECT_SOURCE_DIR}/im/pinyin/chaizi.txt" LibIME::pinyindict
    VERBATIM)
add_custom_target(chinese-llm-native-data ALL DEPENDS "${_data_root}/chaizi.dict")
install(FILES "${_data_root}/chaizi.dict" DESTINATION "${FCITX_INSTALL_PKGDATADIR}/chinese-llm"
    COMPONENT chinese-llm)

# Object-library usage requirements are explicitly propagated at the consumer.
set(CHINESE_LLM_NATIVE_INCLUDE_DIRS ${_native_include_dirs} ${Boost_INCLUDE_DIRS} PARENT_SCOPE)
