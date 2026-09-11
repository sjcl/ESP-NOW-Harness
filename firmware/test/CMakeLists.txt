cmake_minimum_required(VERSION 3.16)
project(espnow_harness_pure_tests C)
enable_testing()
add_executable(protocol_test test_protocol.c ../main/protocol.c)
target_include_directories(protocol_test PRIVATE ../main)
add_test(NAME protocol_test COMMAND protocol_test)

