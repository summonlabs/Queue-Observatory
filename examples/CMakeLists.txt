# Queue Observatory examples.
#
# Each example is a complete, runnable program that demonstrates one part of the
# observation-only boundary.

add_executable(qobs-example-observe observe_queue.cpp)
target_link_libraries(qobs-example-observe PRIVATE qobs_runtime qobs_ingest)

add_executable(qobs-example-policy custom_policy.cpp)
target_link_libraries(qobs-example-policy PRIVATE qobs_runtime)

add_executable(qobs-example-history tail_history.cpp)
target_link_libraries(qobs-example-history PRIVATE qobs_runtime qobs_ingest)
