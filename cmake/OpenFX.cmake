include_guard(GLOBAL)

get_filename_component(
  CORRIDORKEY_DEFAULT_OPENFX_SDK_ROOT
  "${CMAKE_SOURCE_DIR}/third_party/openfx"
  ABSOLUTE
)
set(OPENFX_SDK_ROOT "${CORRIDORKEY_DEFAULT_OPENFX_SDK_ROOT}" CACHE PATH
  "Path to an official AcademySoftwareFoundation/openfx checkout or copied SDK"
)

set(CORRIDORKEY_OPENFX_REQUIRED_HEADERS
  "include/ofxCore.h"
  "include/ofxImageEffect.h"
  "include/ofxParam.h"
  "include/ofxProperty.h"
  "Support/include/ofxsImageEffect.h"
)

function(corridorkey_find_openfx)
  if(NOT IS_ABSOLUTE "${OPENFX_SDK_ROOT}")
    get_filename_component(OPENFX_SDK_ROOT "${OPENFX_SDK_ROOT}" ABSOLUTE
      BASE_DIR "${CMAKE_SOURCE_DIR}"
    )
    set(OPENFX_SDK_ROOT "${OPENFX_SDK_ROOT}" CACHE PATH
      "Path to an official AcademySoftwareFoundation/openfx checkout or copied SDK"
      FORCE
    )
  endif()

  set(openfx_root_display "custom OPENFX_SDK_ROOT (absolute path redacted)")
  if(OPENFX_SDK_ROOT STREQUAL CORRIDORKEY_DEFAULT_OPENFX_SDK_ROOT)
    set(openfx_root_display "third_party/openfx")
  endif()

  unset(CORRIDORKEY_OPENFX_MISSING_HEADERS)
  foreach(required_header IN LISTS CORRIDORKEY_OPENFX_REQUIRED_HEADERS)
    if(NOT EXISTS "${OPENFX_SDK_ROOT}/${required_header}")
      list(APPEND CORRIDORKEY_OPENFX_MISSING_HEADERS "${required_header}")
    endif()
  endforeach()

  if(CORRIDORKEY_OPENFX_MISSING_HEADERS)
    string(REPLACE ";" "\n  " CORRIDORKEY_OPENFX_MISSING_HEADERS_TEXT
      "${CORRIDORKEY_OPENFX_MISSING_HEADERS}"
    )
    message(FATAL_ERROR
      "Official OpenFX SDK headers were not found at OPENFX_SDK_ROOT='${openfx_root_display}'.\n"
      "Run python3 scripts/fetch_openfx_sdk.py to fetch "
      "https://github.com/AcademySoftwareFoundation/openfx.git into third_party/openfx, "
      "or pass -DOPENFX_SDK_ROOT=/path/to/openfx with the same official layout.\n"
      "Missing:\n  ${CORRIDORKEY_OPENFX_MISSING_HEADERS_TEXT}\n"
      "Do not add fake OpenFX API stubs."
    )
  endif()

  if(NOT TARGET OpenFX::OpenFX)
    add_library(OpenFX::OpenFX INTERFACE IMPORTED)
    set_target_properties(OpenFX::OpenFX PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES
        "${OPENFX_SDK_ROOT}/include;${OPENFX_SDK_ROOT}/Support/include"
    )
  endif()

  set(OPENFX_SDK_ROOT "${OPENFX_SDK_ROOT}" PARENT_SCOPE)
  message(STATUS "OpenFX SDK root: ${openfx_root_display}")
endfunction()
