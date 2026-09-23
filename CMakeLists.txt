cmake_minimum_required(VERSION 3.20)

project(QueueObservatory
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral observation and historical interpretation of network queue state"
  HOMEPAGE_URL "https://github.com/summonlabs/Queue-Observatory"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(QOBS_BUILD_TESTS      "Build the Queue Observatory test suite"        ON)
option(QOBS_BUILD_EXAMPLES   "Build Queue Observatory examples"              ON)
option(QOBS_BUILD_BENCHMARKS "Build Queue Observatory benchmarks"            ON)
option(QOBS_BUILD_TOOLS      "Build the qobs CLI, transport server and client" ON)
option(QOBS_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(QOBS_ENABLE_ASAN      "Build with AddressSanitizer when the toolchain supports it" OFF)
option(QOBS_ENABLE_LOCK_AUDIT "Compile the lock-rank / reentrancy audit instrumentation" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning configuration (first-party warning count must be zero)
# ---------------------------------------------------------------------------
add_library(qobs_warnings INTERFACE)
add_library(QueueObservatory::Warnings ALIAS qobs_warnings)

if(MSVC)
  target_compile_options(qobs_warnings INTERFACE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /MP
    /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547 /w14548
    /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928 /w15262)
  if(QOBS_WARNINGS_AS_ERRORS)
    target_compile_options(qobs_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(qobs_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  target_compile_options(qobs_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wcast-qual -Wcast-align -Wnon-virtual-dtor -Woverloaded-virtual
    -Wdouble-promotion -Wformat=2 -Wundef -Wnull-dereference -Wswitch-enum
    -Wno-unknown-warning-option)
  if(QOBS_WARNINGS_AS_ERRORS)
    target_compile_options(qobs_warnings INTERFACE -Werror)
  endif()
endif()

if(QOBS_ENABLE_LOCK_AUDIT)
  target_compile_definitions(qobs_warnings INTERFACE QOBS_ENABLE_LOCK_AUDIT=1)
else()
  target_compile_definitions(qobs_warnings INTERFACE QOBS_ENABLE_LOCK_AUDIT=0)
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer
# ---------------------------------------------------------------------------
if(QOBS_ENABLE_ASAN)
  if(MSVC)
    # The sanitizer needs debug information to report a source location; the
    # compiler emits warning C5072 without it.
    target_compile_options(qobs_warnings INTERFACE /fsanitize=address /Zi /Od)
    target_link_options(qobs_warnings INTERFACE /INCREMENTAL:NO /DEBUG)
  else()
    target_compile_options(qobs_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(qobs_warnings INTERFACE -fsanitize=address)
  endif()
  message(STATUS "Queue Observatory: AddressSanitizer enabled")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(QOBS_CORE_SOURCES
  src/core/Checked.cpp
  src/core/Crc32c.cpp
  src/core/Json.cpp
  src/core/Limits.cpp
  src/core/LockAudit.cpp
  src/core/Result.cpp
  src/core/StrongId.cpp
  src/core/Text.cpp
  src/core/Time.cpp
  src/core/Version.cpp
  src/model/Counter.cpp
  src/model/Event.cpp
  src/model/Evidence.cpp
  src/model/Identity.cpp
  src/model/Metadata.cpp
  src/model/Sample.cpp)

set(QOBS_POLICY_SOURCES
  src/policy/Classification.cpp
  src/policy/Explanation.cpp
  src/policy/Policy.cpp)

set(QOBS_STORE_SOURCES
  src/store/Correlation.cpp
  src/store/History.cpp
  src/store/Store.cpp
  src/store/Window.cpp)

set(QOBS_PERSIST_SOURCES
  src/persist/Format.cpp
  src/persist/Persistence.cpp)

set(QOBS_INGEST_SOURCES
  src/ingest/Decode.cpp
  src/ingest/Ingest.cpp)

set(QOBS_RUNTIME_SOURCES
  src/runtime/Engine.cpp
  src/runtime/Export.cpp
  src/runtime/Observatory.cpp
  src/runtime/Query.cpp)

set(QOBS_TRANSPORT_SOURCES
  src/transport/Client.cpp
  src/transport/Protocol.cpp
  src/transport/Server.cpp
  src/transport/Socket.cpp)

add_library(qobs_core    STATIC ${QOBS_CORE_SOURCES})
add_library(qobs_policy  STATIC ${QOBS_POLICY_SOURCES})
add_library(qobs_store   STATIC ${QOBS_STORE_SOURCES})
add_library(qobs_persist STATIC ${QOBS_PERSIST_SOURCES})
add_library(qobs_ingest  STATIC ${QOBS_INGEST_SOURCES})
add_library(qobs_runtime STATIC ${QOBS_RUNTIME_SOURCES})
add_library(qobs_transport STATIC ${QOBS_TRANSPORT_SOURCES})

set(QOBS_ALL_LIBS qobs_core qobs_policy qobs_store qobs_persist qobs_ingest qobs_runtime qobs_transport)

foreach(lib IN LISTS QOBS_ALL_LIBS)
  target_include_directories(${lib}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${lib} PUBLIC QueueObservatory::Warnings)
  set_target_properties(${lib} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "${lib}")
endforeach()

target_link_libraries(qobs_policy  PUBLIC qobs_core)
target_link_libraries(qobs_store   PUBLIC qobs_core qobs_policy)
target_link_libraries(qobs_persist PUBLIC qobs_core qobs_store)
target_link_libraries(qobs_ingest  PUBLIC qobs_core qobs_store)
target_link_libraries(qobs_runtime PUBLIC qobs_core qobs_policy qobs_store qobs_persist qobs_ingest)
target_link_libraries(qobs_transport PUBLIC qobs_runtime)

find_package(Threads REQUIRED)
foreach(lib IN LISTS QOBS_ALL_LIBS)
  target_link_libraries(${lib} PUBLIC Threads::Threads)
endforeach()

if(WIN32)
  target_link_libraries(qobs_transport PUBLIC ws2_32)
endif()

# Exported names. A downstream consumer writes QueueObservatory::Runtime, not
# the internal target name, so the public surface does not leak build detail.
set_target_properties(qobs_core      PROPERTIES EXPORT_NAME Core)
set_target_properties(qobs_policy    PROPERTIES EXPORT_NAME Policy)
set_target_properties(qobs_store     PROPERTIES EXPORT_NAME Store)
set_target_properties(qobs_persist   PROPERTIES EXPORT_NAME Persist)
set_target_properties(qobs_ingest    PROPERTIES EXPORT_NAME Ingest)
set_target_properties(qobs_runtime   PROPERTIES EXPORT_NAME Runtime)
set_target_properties(qobs_transport PROPERTIES EXPORT_NAME Transport)
set_target_properties(qobs_warnings  PROPERTIES EXPORT_NAME Warnings)

add_library(QueueObservatory::Core      ALIAS qobs_core)
add_library(QueueObservatory::Policy    ALIAS qobs_policy)
add_library(QueueObservatory::Store     ALIAS qobs_store)
add_library(QueueObservatory::Persist   ALIAS qobs_persist)
add_library(QueueObservatory::Ingest    ALIAS qobs_ingest)
add_library(QueueObservatory::Runtime   ALIAS qobs_runtime)
add_library(QueueObservatory::Transport ALIAS qobs_transport)

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS ${QOBS_ALL_LIBS} qobs_warnings
  EXPORT QueueObservatoryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT QueueObservatoryTargets
  FILE QueueObservatoryTargets.cmake
  NAMESPACE QueueObservatory::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QueueObservatory)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/QueueObservatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/QueueObservatoryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QueueObservatory)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/QueueObservatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/QueueObservatoryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/QueueObservatoryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QueueObservatory)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(QOBS_BUILD_TOOLS)
  add_executable(qobs apps/qobs/main.cpp)
  target_link_libraries(qobs PRIVATE qobs_runtime qobs_ingest qobs_persist)
  set_target_properties(qobs PROPERTIES OUTPUT_NAME qobs)

  add_executable(qobs-transport-server apps/qobs_transport_server/main.cpp)
  target_link_libraries(qobs-transport-server PRIVATE qobs_transport)
  set_target_properties(qobs-transport-server PROPERTIES OUTPUT_NAME qobs-transport-server)

  add_executable(qobs-transport-client apps/qobs_transport_client/main.cpp)
  target_link_libraries(qobs-transport-client PRIVATE qobs_transport)
  set_target_properties(qobs-transport-client PROPERTIES OUTPUT_NAME qobs-transport-client)

  install(TARGETS qobs qobs-transport-server qobs-transport-client
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Examples / benchmarks
# ---------------------------------------------------------------------------
if(QOBS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(QOBS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(QOBS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "Queue Observatory ${PROJECT_VERSION}: tests=${QOBS_BUILD_TESTS} examples=${QOBS_BUILD_EXAMPLES} benchmarks=${QOBS_BUILD_BENCHMARKS} asan=${QOBS_ENABLE_ASAN}")
