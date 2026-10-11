# Host unit tests for espix_proc's pure logic, on IDF's linux target: no board,
# and nothing here is linked into the firmware. Run with `make unit`.
cmake_minimum_required(VERSION 3.22)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espix_proc_host_test)
