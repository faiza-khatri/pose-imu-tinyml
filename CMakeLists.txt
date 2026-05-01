# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blinky)

set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)

file(GLOB app_sources src/*)

if(CONFIG_USE_CNN_MODEL)
    file(GLOB model_sources models/cnn/*)
    message(STATUS "Using CNN model")
else()
    file(GLOB model_sources models/dense/*)
    message(STATUS "Using Dense model")
endif()

target_sources(app PRIVATE ${app_sources} ${model_sources})