cmake_minimum_required(VERSION 2.8)

project(event)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wunused -Werror")

file(GLOB SOURCES src/*.c)

# Shared library
add_library(event SHARED ${SOURCES})
target_include_directories(event PUBLIC src)
