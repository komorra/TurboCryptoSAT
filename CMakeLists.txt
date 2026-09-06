cmake_minimum_required(VERSION 3.15)
project(TurboCryptoSAT VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(turbocryptosat
    src/main.cpp
    src/cdcl.cpp
    src/cnf.cpp
    src/gates.cpp
    src/genbench.cpp
    src/platform.cpp
    src/propagator.cpp
    src/signatures.cpp
    src/solver.cpp
    src/tune.cpp
    src/ui.cpp
)

target_include_directories(turbocryptosat PRIVATE src)

if(MSVC)
    target_compile_options(turbocryptosat PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(turbocryptosat PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(turbocryptosat PRIVATE -Wall -Wextra)
    target_compile_options(turbocryptosat PRIVATE $<$<CONFIG:Release>:-O2>)
endif()

find_package(Threads REQUIRED)
target_link_libraries(turbocryptosat PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(turbocryptosat PRIVATE psapi)
endif()

install(TARGETS turbocryptosat RUNTIME DESTINATION bin)

# Two quick checks; the real test suite is the benchmark runner, which
# re-verifies every solution it produces. See benchmark.sh / benchmark.bat.
enable_testing()
add_test(NAME help COMMAND turbocryptosat --help)
add_test(NAME smoke
         COMMAND turbocryptosat
                 ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/09-circuit-i24-g300.cnf
                 --no-ui --siglen 256 --timeout 60)
