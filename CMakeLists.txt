cmake_minimum_required(VERSION 3.16)
project(cinder)

set(CMAKE_CXX_STANDARD 20)

# add_compile_options(-fsanitize=address)
# add_link_options(-fsanitize=address)

find_package(Vulkan REQUIRED)

include_directories(
        deps/
        deps/assimp/include/
        deps/glfw/include/
        deps/glm/
        deps/imGuIZMO.quat/
        cmake-build-debug/deps/assimp/include/
        .
)

set(ALL_LIBS
        Vulkan::Vulkan
        glfw
        assimp
)

# glfw

set(GLFW_BUILD_DOCS OFF CACHE BOOL "GLFW build documentation" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "GLFW build documentation" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "GLFW build documentation" FORCE)
add_subdirectory(deps/glfw)

# imgui

file(GLOB IMGUI_SRCS
        "deps/imgui/*.h"
        "deps/imgui/*.cpp"
)

set(IMGUI_IMPL_SRCS
        deps/imgui/backends/imgui_impl_glfw.h
        deps/imgui/backends/imgui_impl_glfw.cpp
        deps/imgui/backends/imgui_impl_vulkan.h
        deps/imgui/backends/imgui_impl_vulkan.cpp
)

set_source_files_properties(
        ${IMGUI_SRCS}
        ${IMGUI_IMPL_SRCS}
        PROPERTIES COMPILE_FLAGS "-w"
)

# assimp

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "Assimp build tests" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "Assimp install" FORCE)
set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "Assimp install PDB" FORCE)
add_subdirectory(deps/assimp)

# imguizmo.quat

set(IMGUIZMO_QUAT_SRCS
    deps/imGuIZMO.quat/imGuIZMO.quat/imGuIZMOquat.cpp
)

# vk-bootstrap

set(VK_BOOTSTRAP_SRCS
    deps/vk-bootstrap/VkBootstrap.cpp
)

# header-only deps

file(GLOB HEADER_ONLY_DEPS_SRCS
        "deps/stb/*"
        "deps/vma/*"
        "deps/SPIRV-Reflect/spirv_reflect.cpp"
)

# source files

file(GLOB RAYZOR_SRCS
        "src/*"
        "src/render/*"
        "src/render/vk/*"
        "src/render/gui/*"
        "src/render/mesh/*"
        "src/utils/*"
)

add_executable(cinder
        ${RAYZOR_SRCS}
        ${IMGUI_SRCS}
        ${IMGUI_IMPL_SRCS}
        ${IMGUIZMO_QUAT_SRCS}
        ${VK_BOOTSTRAP_SRCS}
        ${HEADER_ONLY_DEPS_SRCS})
target_link_libraries(cinder ${ALL_LIBS})
