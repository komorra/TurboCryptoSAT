@echo off
rem Builds TurboCryptoSAT. Uses CMake when available, otherwise g++ directly.
setlocal
cd /d "%~dp0"

where cmake >nul 2>&1
if %errorlevel%==0 (
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release || exit /b 1
    cmake --build build --config Release || exit /b 1
    echo built: build\Release\turbocryptosat.exe
    exit /b 0
)

echo cmake not found, compiling directly with g++
if not exist build mkdir build
g++ -std=c++17 -O2 -Wall -Wextra -pthread ^
    src\main.cpp src\cdcl.cpp src\cnf.cpp src\gates.cpp src\genbench.cpp src\platform.cpp ^
    src\propagator.cpp src\signatures.cpp src\solver.cpp src\tune.cpp src\ui.cpp ^
    -o build\turbocryptosat.exe -lpsapi || exit /b 1
echo built: build\turbocryptosat.exe
