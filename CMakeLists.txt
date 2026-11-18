cmake_minimum_required(VERSION 3.20)
project(ChromaTranslate VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/onnxruntime")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ONNXRUNTIME_ROOT}/include
)

link_directories(
    ${ONNXRUNTIME_ROOT}/lib
)

set(SOURCES
    src/main.cpp
    src/HotKeyManager.cpp
    src/SimpleCapture.cpp
    src/PaddleOcrEngine.cpp
    src/TranslationRouter.cpp
    src/TrayAndSettingsUI.cpp
)

add_executable(ChromaTranslate WIN32 ${SOURCES})

target_link_libraries(ChromaTranslate PRIVATE
    d3d11.lib
    dxgi.lib
    d2d1.lib
    dwrite.lib
    dcomp.lib
    winhttp.lib
    dwmapi.lib
    shcore.lib
    shell32.lib
    comctl32.lib
    uxtheme.lib
    windowsapp.lib
    onnxruntime.lib
)

add_custom_command(TARGET ChromaTranslate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        $<TARGET_FILE_DIR:ChromaTranslate>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
        $<TARGET_FILE_DIR:ChromaTranslate>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:ChromaTranslate>/models"
)