cmake_minimum_required(VERSION 3.20)

project(CorridorKeyOFX
  VERSION 0.1.0
  DESCRIPTION "CorridorKey OpenFX plugin and local inference sidecar"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(Python3 3.7 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OpenFX)

corridorkey_find_openfx()

add_library(corridorkey_core STATIC
  src/core/AlphaHint.cpp
  src/core/ColorAlphaContract.cpp
  src/core/ImageFormat.cpp
  src/core/PixelConvert.cpp
  src/core/RenderWindow.cpp
  src/core/StatusFrame.cpp
)
target_include_directories(corridorkey_core PUBLIC src)
set_target_properties(corridorkey_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

add_library(corridorkey_sidecar_client STATIC
  src/sidecar_client/SidecarClient.cpp
  src/sidecar_client/SidecarProcess.cpp
)
target_include_directories(corridorkey_sidecar_client PUBLIC src)
target_link_libraries(corridorkey_sidecar_client PUBLIC Threads::Threads)
set_target_properties(corridorkey_sidecar_client PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

add_library(corridorkey_ofx_objects OBJECT
  src/ofx/CorridorKeyPlugin.cpp
  src/ofx/OfxDescribe.cpp
  src/ofx/OfxParameters.cpp
  src/ofx/OfxRender.cpp
)
target_link_libraries(corridorkey_ofx_objects PRIVATE
  OpenFX::OpenFX
  corridorkey_core
  corridorkey_sidecar_client
  ${CMAKE_DL_LIBS}
)
target_include_directories(corridorkey_ofx_objects PRIVATE src/ofx src)
set_target_properties(corridorkey_ofx_objects PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

add_library(CorridorKeyOFX MODULE $<TARGET_OBJECTS:corridorkey_ofx_objects>)
target_link_libraries(CorridorKeyOFX PRIVATE
  OpenFX::OpenFX
  corridorkey_core
  corridorkey_sidecar_client
  ${CMAKE_DL_LIBS}
)
set_target_properties(CorridorKeyOFX PROPERTIES
  PREFIX ""
  OUTPUT_NAME "CorridorKey"
  SUFFIX ".ofx"
)

add_executable(test_bundle_layout tests/cpp/test_bundle_layout.cpp)
target_compile_definitions(test_bundle_layout PRIVATE
  CORRIDORKEY_BUNDLE_DIRECTORY="CorridorKey.ofx.bundle"
  CORRIDORKEY_BUNDLE_BINARY="CorridorKey.ofx"
  CORRIDORKEY_BUILD_BUNDLE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/packaging/build_bundle.py"
  CORRIDORKEY_PLUGIN_BINARY="$<TARGET_FILE:CorridorKeyOFX>"
  CORRIDORKEY_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
)
add_dependencies(test_bundle_layout CorridorKeyOFX)
add_test(NAME bundle_layout COMMAND test_bundle_layout)
set_tests_properties(bundle_layout PROPERTIES
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
  NAME sidecar_python_unittests
  COMMAND ${Python3_EXECUTABLE} -m unittest discover -s sidecar/tests -p "test_*.py"
)
set_tests_properties(sidecar_python_unittests PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

function(add_corridorkey_cpp_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE corridorkey_core OpenFX::OpenFX)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

function(add_corridorkey_sidecar_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE corridorkey_sidecar_client ${ARGN})
  target_compile_definitions(${target} PRIVATE
    CORRIDORKEY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CORRIDORKEY_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
  )
  add_test(NAME ${target} COMMAND ${target})
endfunction()

function(add_corridorkey_ofx_test target test_name source)
  add_executable(${target}
    ${source}
    $<TARGET_OBJECTS:corridorkey_ofx_objects>
  )
  target_link_libraries(${target} PRIVATE
    OpenFX::OpenFX
    corridorkey_core
    corridorkey_sidecar_client
    ${CMAKE_DL_LIBS}
  )
  target_include_directories(${target} PRIVATE src/ofx src)
  target_compile_definitions(${target} PRIVATE
    CORRIDORKEY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CORRIDORKEY_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
  )
  add_test(NAME ${test_name} COMMAND ${target})
endfunction()

add_corridorkey_cpp_test(test_image_format tests/cpp/test_image_format.cpp)
add_corridorkey_cpp_test(test_alpha_hint tests/cpp/test_alpha_hint.cpp)
add_corridorkey_cpp_test(test_pixel_convert tests/cpp/test_pixel_convert.cpp)
add_corridorkey_cpp_test(test_render_window tests/cpp/test_render_window.cpp)
add_corridorkey_cpp_test(test_status_frame tests/cpp/test_status_frame.cpp)
add_corridorkey_cpp_test(test_color_alpha_contract tests/cpp/test_color_alpha_contract.cpp)

add_executable(test_sidecar_protocol tests/cpp/test_sidecar_protocol.cpp)
target_link_libraries(test_sidecar_protocol PRIVATE corridorkey_sidecar_client)
add_test(NAME test_sidecar_protocol COMMAND test_sidecar_protocol)

add_corridorkey_sidecar_test(
  test_oom_status
  tests/cpp/test_oom_status.cpp
)

add_corridorkey_sidecar_test(
  test_sidecar_launch_contract
  tests/cpp/test_sidecar_launch_contract.cpp
)
add_corridorkey_sidecar_test(
  test_sidecar_failure
  tests/cpp/test_sidecar_failure.cpp
  corridorkey_core
  OpenFX::OpenFX
)
add_corridorkey_sidecar_test(
  test_abort_polling
  tests/cpp/test_abort_polling.cpp
)

add_corridorkey_ofx_test(
  test_ofx_descriptor
  ofx_descriptor
  tests/cpp/test_ofx_descriptor.cpp
)
add_corridorkey_ofx_test(
  test_ofx_general_context
  ofx_general_context
  tests/cpp/test_ofx_general_context.cpp
)
add_corridorkey_ofx_test(
  test_ofx_clip_preferences
  ofx_clip_preferences
  tests/cpp/test_ofx_clip_preferences.cpp
)
add_corridorkey_ofx_test(
  test_request_contract
  request_contract
  tests/cpp/test_request_contract.cpp
)

add_executable(test_ofx_binary_scan tests/cpp/test_ofx_binary_scan.cpp)
target_link_libraries(test_ofx_binary_scan PRIVATE OpenFX::OpenFX ${CMAKE_DL_LIBS})
target_compile_definitions(test_ofx_binary_scan PRIVATE
  CORRIDORKEY_PLUGIN_BINARY="$<TARGET_FILE:CorridorKeyOFX>"
)
add_dependencies(test_ofx_binary_scan CorridorKeyOFX)
add_test(NAME ofx_binary_scan COMMAND test_ofx_binary_scan)

add_test(
  NAME ofx_binary_scanner_strings_report
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_ofx_binary_scanner_strings.py
          $<TARGET_FILE:CorridorKeyOFX>
)
set_tests_properties(ofx_binary_scanner_strings_report PROPERTIES
  SKIP_RETURN_CODE 77
)
add_corridorkey_ofx_test(
  test_output_modes
  output_modes
  tests/cpp/test_output_modes.cpp
)
