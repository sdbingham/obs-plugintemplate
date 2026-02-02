cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(ENABLE_WHISPER "Use whisper.cpp for transcription (Phase 2)" OFF)
option(ENABLE_WHISPER_CUDA "Build whisper.cpp with CUDA (requires CUDA Toolkit)" OFF)

set(ENABLE_FRONTEND_API_INT 0)
set(ENABLE_QT_INT 0)
set(ENABLE_WHISPER_INT 0)
if(ENABLE_FRONTEND_API)
  set(ENABLE_FRONTEND_API_INT 1)
endif()
if(ENABLE_QT)
  set(ENABLE_QT_INT 1)
endif()
if(ENABLE_WHISPER)
  set(ENABLE_WHISPER_INT 1)
endif()
configure_file(src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/transcription/transcription.module.c
    src/transcription/transcription.service.c
    src/audio/audio.module.c
    src/audio/audio.service.c
    src/captions/captions.module.c
    src/captions/captions.service.c
    src/frontend/frontend.module.c
    src/frontend/frontend.service.c
)

target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

if(ENABLE_QT)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/frontend/frontend_settings_ui.cpp
  )
endif()

if(ENABLE_WHISPER)
  set(GGML_CUDA ${ENABLE_WHISPER_CUDA} CACHE BOOL "Build ggml with CUDA" FORCE)
  set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(WHISPER_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  # Subproject inherits our CMAKE_COMPILE_WARNING_AS_ERROR; turn off so whisper.cpp
  # builds without /WX (it has many MSVC conversion warnings).
  set(_saved_compile_warning_as_error "${CMAKE_COMPILE_WARNING_AS_ERROR}")
  set(CMAKE_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
  include(FetchContent)
  FetchContent_Declare(
    whisper.cpp
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp
    GIT_TAG        v1.8.3
    GIT_SUBMODULES "ggml"
  )
  FetchContent_MakeAvailable(whisper.cpp)
  set(CMAKE_COMPILE_WARNING_AS_ERROR "${_saved_compile_warning_as_error}" CACHE BOOL "" FORCE)
  unset(_saved_compile_warning_as_error)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/transcription/whisper_wrapper.cpp
  )
  target_include_directories(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      "${whisper.cpp_SOURCE_DIR}/include"
  )
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
