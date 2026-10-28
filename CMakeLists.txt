cmake_minimum_required(VERSION 3.20)
project(BananaInternal VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
    message(FATAL_ERROR "A Windows x64 MSVC-compatible toolchain is required")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(project_options target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
endfunction()

add_subdirectory(vendor/minhook EXCLUDE_FROM_ALL)
add_library(imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC vendor/imgui vendor/imgui/backends)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(BananaUI STATIC src/ui.cpp src/render_hook.cpp)
target_include_directories(BananaUI PUBLIC src PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BananaUI PUBLIC imgui minhook d3d11 dxgi d3dcompiler dwmapi user32 gdi32)
project_options(BananaUI)

add_library(BananaInternal SHARED src/main.cpp src/silent_aim.cpp src/weapon_features.cpp)
set_target_properties(BananaInternal PROPERTIES OUTPUT_NAME BananaInternalUI)
target_include_directories(BananaInternal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BananaInternal PRIVATE BananaUI bcrypt)
project_options(BananaInternal)

add_executable(BananaLoader src/loader.cpp)
project_options(BananaLoader)

include(CTest)
if(BUILD_TESTING)
    add_executable(MathChecks tests/aim_math.cpp)
    target_include_directories(MathChecks PRIVATE src)
    project_options(MathChecks)
    add_test(NAME AimMath COMMAND MathChecks)

    add_executable(RendererChecks tests/render_input.cpp)
    target_link_libraries(RendererChecks PRIVATE BananaUI)
    project_options(RendererChecks)
    add_test(NAME RendererInput COMMAND RendererChecks)
    set_tests_properties(RendererInput PROPERTIES TIMEOUT 25 LABELS "d3d11")
endif()
