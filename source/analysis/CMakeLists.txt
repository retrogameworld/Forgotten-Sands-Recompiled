cmake_minimum_required(VERSION 3.20)
project(fsanalysis CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_subdirectory(<project folder>/tools/XenonRecomp xenonrecomp EXCLUDE_FROM_ALL)

add_executable(xexdiag xexdiag/main.cpp)
target_link_libraries(xexdiag PRIVATE XenonUtils fmt::fmt)

add_executable(fsanalyse fsanalyse/main.cpp)
target_link_libraries(fsanalyse PRIVATE XenonUtils fmt::fmt)
