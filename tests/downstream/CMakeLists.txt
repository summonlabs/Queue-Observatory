# Independent downstream consumer.
#
# This is deliberately NOT part of the Queue Observatory build. It exists to
# prove the installed package can be found and used by a project that shares no
# build tree with the library, using only find_package and the exported targets.
#
#   cmake --install <build-dir> --config Release --prefix <prefix>
#   cmake -S tests/downstream -B <consumer-build> -DCMAKE_PREFIX_PATH=<prefix>
#   cmake --build <consumer-build> --config Release
#   <consumer-build>/Release/qobs-downstream-consumer

cmake_minimum_required(VERSION 3.20)

project(QueueObservatoryDownstreamConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent consumer that proves the installed Queue Observatory package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(QueueObservatory 1.0 CONFIG REQUIRED)

add_executable(qobs-downstream-consumer main.cpp)
target_link_libraries(qobs-downstream-consumer PRIVATE
  QueueObservatory::Runtime
  QueueObservatory::Ingest
  QueueObservatory::Persist
  QueueObservatory::Transport)
