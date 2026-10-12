# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_text WIN32)
init_target(test_text "(tests)")

target_include_directories(test_text PRIVATE ${src_loc})

nice_target_sources(test_text ${src_loc}
PRIVATE
    tests/test_main.cpp
    tests/test_main.h
    tests/test_text.cpp
)

nice_target_sources(test_text ${res_loc}
PRIVATE
    qrc/emoji_1.qrc
    qrc/emoji_2.qrc
    qrc/emoji_3.qrc
    qrc/emoji_4.qrc
    qrc/emoji_5.qrc
    qrc/emoji_6.qrc
    qrc/emoji_7.qrc
    qrc/emoji_8.qrc
)

target_link_libraries(test_text
PRIVATE
    desktop-app::lib_base
    desktop-app::lib_crl
    desktop-app::lib_ui
    desktop-app::external_qt
    desktop-app::external_qt_static_plugins
)

set_target_properties(test_text PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_dependencies(Telegram test_text)

target_prepare_qrc(test_text)

if (APPLE)
    add_custom_command(TARGET test_text POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:test_text>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/test_text.rcc"
            "${CMAKE_BINARY_DIR}/lib_ui.rcc"
            "$<TARGET_FILE_DIR:test_text>/Contents/Resources/"
    )
endif()

enable_testing()
add_executable(test_blur_transition ${src_loc}/tests/test_blur_transition.cpp)
target_include_directories(test_blur_transition PRIVATE ${src_loc})
target_compile_features(test_blur_transition PRIVATE cxx_std_20)
add_test(NAME blur_transition COMMAND test_blur_transition)

if (WIN32)
    add_executable(test_windows_acrylic ${src_loc}/tests/test_windows_acrylic.cpp)
    target_compile_features(test_windows_acrylic PRIVATE cxx_std_20)
    target_link_libraries(test_windows_acrylic PRIVATE dwmapi user32)
    add_test(NAME windows_acrylic COMMAND test_windows_acrylic)
    set_tests_properties(windows_acrylic PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(test_blur_kernel ${src_loc}/tests/test_blur_kernel.cpp)
target_include_directories(test_blur_kernel PRIVATE ${src_loc})
target_compile_features(test_blur_kernel PRIVATE cxx_std_20)
add_test(NAME blur_kernel COMMAND test_blur_kernel)
