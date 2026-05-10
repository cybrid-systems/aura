cmake_minimum_required(VERSION 4.3)
# Enable experimental import std; BEFORE project()
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
project(demo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
add_executable(demo main.cpp)
set_property(TARGET demo PROPERTY CXX_MODULE_STD ON)
