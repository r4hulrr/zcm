cmake_minimum_required(VERSION 3.16)
project(zcm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)           
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_library(RT_LIBRARY rt)

add_library(zcm INTERFACE)
target_include_directories(zcm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
if(RT_LIBRARY)
    target_link_libraries(zcm INTERFACE ${RT_LIBRARY})
endif()

# ---------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------
add_executable(fifoTest tests/twoProcessFifoTest.cpp)
target_link_libraries(fifoTest PRIVATE zcm)
target_include_directories(fifoTest PRIVATE tests)     # sharedLayout.hpp lives here

add_executable(stress tests/stressTwoProcess.cpp)
target_link_libraries(stress PRIVATE zcm)              # defines its own tiny layout

add_executable(pubsubDemo tests/pubsubDemo.cpp)
target_link_libraries(pubsubDemo PRIVATE zcm)
target_include_directories(pubsubDemo PRIVATE tests)

add_executable(harness harness/latencyHarness.cpp)
target_link_libraries(harness PRIVATE zcm)
target_include_directories(harness PRIVATE harness)

add_executable(pairRunner tests/pairRunner.cpp)

enable_testing()

add_test(NAME fifo.two_process COMMAND pairRunner $<TARGET_FILE:fifoTest>)
add_test(NAME api.pubsub_demo  COMMAND pairRunner $<TARGET_FILE:pubsubDemo>)
add_test(NAME stress.positive  COMMAND pairRunner $<TARGET_FILE:stress>)

# THE NEGATIVE CONTROL. WILL_FAIL inverts the pass condition, so CTest reports
# "Passed" only when the test binary FAILS. If someone weakens an invariant this
# flips red -- which is the whole point of shipping a negative control.
add_test(NAME stress.negative_control_reconstruct
         COMMAND pairRunner $<TARGET_FILE:stress> --sub --negative=reconstruct)
set_tests_properties(stress.negative_control_reconstruct PROPERTIES WILL_FAIL TRUE)

set_tests_properties(
    fifo.two_process api.pubsub_demo stress.positive
    stress.negative_control_reconstruct
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS "correctness")

# --- benchmarks: excluded from a plain `ctest` run via the label ------------
set(ZCM_RATE  20000 CACHE STRING "harness target message rate (msg/s)")
set(ZCM_COUNT 40000 CACHE STRING "harness recorded sample count")

foreach(t shm shmzc uds)
    add_test(NAME latency.${t} COMMAND pairRunner $<TARGET_FILE:harness>
             --transport=${t} --rate=${ZCM_RATE} --count=${ZCM_COUNT} --warmup=2000
             --sub --csv=${CMAKE_BINARY_DIR}/cdf_${t}.csv)
    set_tests_properties(latency.${t}
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 600 LABELS "bench")
endforeach()

# ---------------------------------------------------------------------------
# `cmake --build build --target chart` -> merged CSV + latency.png
# ---------------------------------------------------------------------------
add_custom_target(chart
    COMMAND ${CMAKE_COMMAND} -E cat ${CMAKE_BINARY_DIR}/cdf_shm.csv   > ${CMAKE_BINARY_DIR}/cdf_all.csv
    COMMAND bash -c "tail -n +2 ${CMAKE_BINARY_DIR}/cdf_shmzc.csv >> ${CMAKE_BINARY_DIR}/cdf_all.csv"
    COMMAND bash -c "tail -n +2 ${CMAKE_BINARY_DIR}/cdf_uds.csv   >> ${CMAKE_BINARY_DIR}/cdf_all.csv"
    COMMAND ${CMAKE_COMMAND} -E echo "merged -> ${CMAKE_BINARY_DIR}/cdf_all.csv"
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/plotCdf.py
            ${CMAKE_BINARY_DIR}/cdf_all.csv ${CMAKE_CURRENT_SOURCE_DIR}/latency.png
    VERBATIM)