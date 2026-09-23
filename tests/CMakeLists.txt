# Queue Observatory test suite.
#
# Every suite runs as its own executable and every suite is registered with
# CTest. No test uses a timeout: a test either reaches its conclusion or the
# process is killed by the operating system, which is what makes a hang a real
# defect instead of a flake.

set(QOBS_TEST_SUPPORT support/TestHarness.cpp)

function(qobs_add_test name)
  add_executable(${name} ${ARGN} ${QOBS_TEST_SUPPORT})
  target_link_libraries(${name} PRIVATE qobs_runtime qobs_transport qobs_persist qobs_ingest)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 0)
endfunction()

qobs_add_test(qobs_unit_tests
  unit/test_core.cpp
  unit/test_counter.cpp
  unit/test_evidence.cpp
  unit/test_policy.cpp
  unit/test_classification.cpp
  unit/test_window.cpp
  unit/test_store.cpp
  unit/test_persistence.cpp
  unit/test_history.cpp)

qobs_add_test(qobs_property_tests
  property/test_property.cpp)

qobs_add_test(qobs_integration_tests
  integration/test_ingest.cpp
  integration/test_runtime.cpp
  integration/test_end_to_end.cpp)

qobs_add_test(qobs_adversarial_tests
  adversarial/test_adversarial.cpp)

qobs_add_test(qobs_concurrency_tests
  concurrency/test_concurrency.cpp)

qobs_add_test(qobs_recovery_tests
  recovery/test_recovery.cpp)

qobs_add_test(qobs_transport_tests
  transport/test_transport.cpp)

# The transport suite needs the real executables so that it can start them as
# independent operating-system processes.
target_compile_definitions(qobs_transport_tests PRIVATE
  QOBS_TRANSPORT_SERVER_EXE="$<TARGET_FILE:qobs-transport-server>"
  QOBS_TRANSPORT_CLIENT_EXE="$<TARGET_FILE:qobs-transport-client>"
  QOBS_CLI_EXE="$<TARGET_FILE:qobs>")
