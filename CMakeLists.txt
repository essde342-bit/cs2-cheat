cmake_minimum_required(VERSION 3.21)
project(VoidDLC LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.90.4)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui stb)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${stb_SOURCE_DIR})
target_link_libraries(imgui_lib PUBLIC d3d11 dxgi)

# --- Payload ---
add_executable(CS2ESP main.cpp)
target_link_libraries(CS2ESP PRIVATE imgui_lib urlmon dwmapi)
target_compile_options(CS2ESP PRIVATE /W3 /O2 /utf-8)
target_link_options(CS2ESP PRIVATE /RELEASE /DEBUG:NONE /OPT:REF /OPT:ICF)

# --- Ресурс: payload + logo ---
set(PAYLOAD_RC ${CMAKE_CURRENT_BINARY_DIR}/payload.rc)
set(RC_CONTENT "1 RCDATA \"${CMAKE_CURRENT_BINARY_DIR}/CS2ESP.exe\"\n")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/logo.png)
    string(APPEND RC_CONTENT "2 RCDATA \"${CMAKE_CURRENT_SOURCE_DIR}/resources/logo.png\"\n")
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/logo.png)
    string(APPEND RC_CONTENT "2 RCDATA \"${CMAKE_CURRENT_SOURCE_DIR}/logo.png\"\n")
endif()
file(GENERATE OUTPUT ${PAYLOAD_RC} CONTENT "${RC_CONTENT}")

# --- Loader ---
add_executable(CS2Loader loader_main.cpp ${PAYLOAD_RC})
add_dependencies(CS2Loader CS2ESP)
target_link_libraries(CS2Loader PRIVATE imgui_lib urlmon wininet)
target_compile_options(CS2Loader PRIVATE /W3 /O2 /utf-8)
target_link_options(CS2Loader PRIVATE /RELEASE /DEBUG:NONE /OPT:REF /OPT:ICF)
