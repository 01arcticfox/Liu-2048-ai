cmake_minimum_required(VERSION 3.20)
project(Game2048AI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WV2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WebView2")

if (MSVC)
  add_compile_options(/Zc:__cplusplus /utf-8 /EHsc /W3 /MP /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------- desktop app
add_executable(Game2048AI WIN32 src/main.cpp)

target_include_directories(Game2048AI PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${WV2_DIR}/include")

target_link_libraries(Game2048AI PRIVATE
  "${WV2_DIR}/x64/WebView2Loader.dll.lib"
  dwmapi shlwapi ole32 user32 gdi32 shell32)

target_compile_definitions(Game2048AI PRIVATE
  "WEB_ASSET_DIR=L\"${CMAKE_CURRENT_SOURCE_DIR}/../web\"")

if (MSVC)
  target_link_options(Game2048AI PRIVATE /SUBSYSTEM:WINDOWS)
endif()

add_custom_command(TARGET Game2048AI POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../web" "$<TARGET_FILE_DIR:Game2048AI>/web"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WV2_DIR}/x64/WebView2Loader.dll" "$<TARGET_FILE_DIR:Game2048AI>")

# ---------------------------------------------------------------- console app
add_executable(ai_console console/main.cpp)
target_include_directories(ai_console PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(ai_console PROPERTIES OUTPUT_NAME 2048ai)
if (MSVC)
  target_compile_options(ai_console PRIVATE /O2)
endif()
