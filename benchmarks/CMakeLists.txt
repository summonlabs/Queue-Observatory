# Queue Observatory benchmarks.
#
# Every benchmark measures completed work: it counts the operations that ran to
# completion and reports the count alongside the elapsed time. Nothing here
# measures an estimate, a projection, or an assumed throughput.

add_executable(qobs-benchmark benchmark_main.cpp)
target_link_libraries(qobs-benchmark PRIVATE qobs_runtime qobs_ingest qobs_persist)

add_executable(qobs-benchmark-transport benchmark_transport.cpp)
target_link_libraries(qobs-benchmark-transport PRIVATE qobs_transport)
