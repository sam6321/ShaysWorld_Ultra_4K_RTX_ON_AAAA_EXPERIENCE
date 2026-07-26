# Package a portable Shays World VK zip.
# Invoked via: cmake --build . --config Release --target package_release
# Or: cmake -DSHAYS_EXE=... -DSHAYS_ASSETS=... -DSHAYS_SHADERS=... -DSHAYS_OUT_DIR=... -P scripts/package_release.cmake

if(NOT SHAYS_EXE OR NOT EXISTS "${SHAYS_EXE}")
  message(FATAL_ERROR "SHAYS_EXE missing or not found: '${SHAYS_EXE}'")
endif()
if(NOT SHAYS_ASSETS OR NOT EXISTS "${SHAYS_ASSETS}/scene.bin")
  message(FATAL_ERROR "SHAYS_ASSETS missing scene.bin: '${SHAYS_ASSETS}'")
endif()
if(NOT SHAYS_SHADERS OR NOT EXISTS "${SHAYS_SHADERS}")
  message(FATAL_ERROR "SHAYS_SHADERS missing: '${SHAYS_SHADERS}'")
endif()
if(NOT SHAYS_OUT_DIR)
  set(SHAYS_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/dist")
endif()

set(STAGE "${SHAYS_OUT_DIR}/shays-world-vk")
file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}/shaders")
file(MAKE_DIRECTORY "${STAGE}/assets")

get_filename_component(_exe_name "${SHAYS_EXE}" NAME)
file(COPY "${SHAYS_EXE}" DESTINATION "${STAGE}")
file(COPY "${SHAYS_SHADERS}/" DESTINATION "${STAGE}/shaders")
file(COPY "${SHAYS_ASSETS}/" DESTINATION "${STAGE}/assets")

if(SHAYS_README AND EXISTS "${SHAYS_README}")
  file(COPY "${SHAYS_README}" DESTINATION "${STAGE}")
endif()

file(WRITE "${STAGE}/CONTROLS.txt"
"Shays World VK — controls
=========================
WASD          walk / fly
Mouse         look
F1            Performance <-> Quality
F2            capture / release cursor
F3            free-fly toggle
F4            hide / show HUD
R             rain (Quality)
L             force walkway lamps (Quality)
Esc           quit

Requires a Vulkan 1.2+ GPU and recent drivers.
")

set(ZIP_PATH "${SHAYS_OUT_DIR}/shays-world-vk-windows.zip")
file(REMOVE "${ZIP_PATH}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E tar cf "${ZIP_PATH}" --format=zip "shays-world-vk"
  WORKING_DIRECTORY "${SHAYS_OUT_DIR}"
  RESULT_VARIABLE _zip_rc)
if(NOT _zip_rc EQUAL 0)
  message(FATAL_ERROR "Failed to create zip (${_zip_rc})")
endif()

message(STATUS "Packed: ${ZIP_PATH}")
