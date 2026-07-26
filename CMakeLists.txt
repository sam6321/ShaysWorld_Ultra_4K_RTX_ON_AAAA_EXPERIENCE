cmake_minimum_required(VERSION 3.24)
project(shays_vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED ENV{VULKAN_SDK} AND EXISTS "C:/VulkanSDK")
  file(GLOB _vk_versions "C:/VulkanSDK/*")
  list(SORT _vk_versions)
  list(GET _vk_versions -1 _latest_vk)
  set(ENV{VULKAN_SDK} "${_latest_vk}")
endif()

find_package(Vulkan REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(vk_bootstrap
  GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
  GIT_TAG v1.3.296
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(vk_bootstrap)

FetchContent_Declare(VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.1.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
set(VMA_INCLUDE_DIR "${vulkanmemoryallocator_SOURCE_DIR}/include")

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.8
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw Vulkan::Vulkan)

# Compile GLSL -> SPIR-V next to sources and into the build output dir.
find_program(GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin")
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})
set(SHADER_LIST
  textured.vert textured.frag
  fullscreen.vert
  shadow.vert shadow.frag
  sky.frag
  rain.vert rain.frag
  ssao.frag ssao_blur.frag
  post.frag taa.frag blit.frag)
foreach(SHADER ${SHADER_LIST})
  add_custom_command(
    OUTPUT ${SHADER_OUT_DIR}/${SHADER}.spv ${SHADER_SRC_DIR}/${SHADER}.spv
    COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER_SRC_DIR}/${SHADER} -o ${SHADER_OUT_DIR}/${SHADER}.spv
    COMMAND ${CMAKE_COMMAND} -E copy ${SHADER_OUT_DIR}/${SHADER}.spv ${SHADER_SRC_DIR}/${SHADER}.spv
    DEPENDS ${SHADER_SRC_DIR}/${SHADER}
    COMMENT "Compiling ${SHADER}")
  list(APPEND SHADER_SPV ${SHADER_OUT_DIR}/${SHADER}.spv)
endforeach()
add_custom_target(shaders ALL DEPENDS ${SHADER_SPV})

add_executable(shays_vk
  src/main.cpp
  src/app.cpp
  src/vk/context.cpp
  src/vk/vma_impl.cpp
  src/settings/graphics_settings.cpp
  src/scene/scene.cpp
  src/render/mesh_pass.cpp
  src/render/quality_renderer.cpp
  src/game/camera.cpp
  src/game/collision.cpp
  src/game/audio.cpp
  src/util/paths.cpp)

add_dependencies(shays_vk shaders)

target_include_directories(shays_vk PRIVATE
  src
  ${VMA_INCLUDE_DIR})

target_link_libraries(shays_vk PRIVATE
  glfw
  glm::glm
  vk-bootstrap::vk-bootstrap
  Vulkan::Vulkan
  imgui_lib)

target_compile_definitions(shays_vk PRIVATE
  GLFW_INCLUDE_VULKAN
  GLM_FORCE_RADIANS
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  SHAYS_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
  SHAYS_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")

if(MSVC)
  target_compile_options(shays_vk PRIVATE /W4 /permissive-)
  # VMA is noisy under /W4
  set_source_files_properties(src/vk/vma_impl.cpp PROPERTIES COMPILE_FLAGS "/W0")
else()
  target_compile_options(shays_vk PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET shays_vk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:shays_vk>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${SHADER_OUT_DIR} $<TARGET_FILE_DIR:shays_vk>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:shays_vk>/assets
  COMMENT "Copying shaders + assets next to shays_vk")

# Convenience target: zip a redistributable next to the binary.
if(WIN32)
  set(_SHAYS_PKG_NAME "shays-world-vk-windows.zip")
else()
  set(_SHAYS_PKG_NAME "shays-world-vk-linux-x86_64.zip")
endif()
add_custom_target(package_release
  COMMAND ${CMAKE_COMMAND}
    -DSHAYS_EXE=$<TARGET_FILE:shays_vk>
    -DSHAYS_ASSETS=${CMAKE_SOURCE_DIR}/assets
    -DSHAYS_SHADERS=${SHADER_OUT_DIR}
    -DSHAYS_OUT_DIR=${CMAKE_BINARY_DIR}/dist
    -DSHAYS_README=${CMAKE_SOURCE_DIR}/README.md
    -DSHAYS_PACKAGE_NAME=${_SHAYS_PKG_NAME}
    -P ${CMAKE_SOURCE_DIR}/scripts/package_release.cmake
  DEPENDS shays_vk
  COMMENT "Packaging Shays World VK release zip"
  VERBATIM)

add_executable(shays_bake
  tools/shays_bake/main.cpp
  tools/shays_bake/mesh_gen.cpp
  tools/shays_bake/raw_texture.cpp)

target_include_directories(shays_bake PRIVATE tools/shays_bake src)
target_compile_features(shays_bake PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(shays_bake PRIVATE /W4 /permissive-)
endif()
