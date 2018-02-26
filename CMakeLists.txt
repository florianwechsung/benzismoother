cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_COMPILER "mpicxx")
set(CMAKE_C_COMPILER "mpicc")

project(ssc)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/modules")
set(CMAKE_CXX_FLAGS "-O2 -mtune=generic -DLINUX -DNDEBUG -fPIC")
set(CMAKE_C_FLAGS "-O2 -mtune=generic -DLINUX -DNDEBUG -fPIC")

set(CMAKE_EXE_LINKER_FLAGS "-mtune=generic")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(${PROJECT_NAME} SHARED
  ssc/libssc.cpp
)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON)

find_package(PETSc REQUIRED)
message(STATUS ${PETSc})

target_link_libraries(${PROJECT_NAME} PRIVATE ${PETSC_LIBRARIES})
target_include_directories(${PROJECT_NAME} PRIVATE ${PETSC_INCLUDES})
