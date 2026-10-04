# CMake version
cmake_minimum_required(VERSION 3.10)

# Project Name
project(GUI)

# C++ 17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 1. Vulkan
find_package(Vulkan REQUIRED)

# 2. GLFW 
add_subdirectory(dependencies/GLFW)

# 3. ImGui
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

# 4. Create Executable
if(WIN32)
    add_executable(${PROJECT_NAME} WIN32
        src/main.cpp 
        dependencies/sqlite/sqlite3.c
        ${IMGUI_SOURCES}
    )
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE "/ENTRY:mainCRTStartup")
    endif()
else()
    add_executable(${PROJECT_NAME}
        src/main.cpp 
        dependencies/sqlite/sqlite3.c
        ${IMGUI_SOURCES}
    )
endif()

# 5. Include Directories
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/sqlite
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies
    ${Vulkan_INCLUDE_DIRS}
)

# 6. Link Libraries
target_link_libraries(${PROJECT_NAME} PRIVATE 
    glfw
    Vulkan::Vulkan
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE dwmapi user32 gdi32 shell32 winhttp)
endif()
