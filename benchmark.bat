@echo off
rem Runs the whole benchmark suite and prints the summary table.
rem
rem   benchmark.bat                 default settings
rem   benchmark.bat --timeout 300   extra flags are forwarded to the solver
setlocal
cd /d "%~dp0"

set BIN=
if exist build\turbocryptosat.exe set BIN=build\turbocryptosat.exe
if exist build\Release\turbocryptosat.exe set BIN=build\Release\turbocryptosat.exe
if "%BIN%"=="" (
    echo solver binary not found; run build.bat first
    exit /b 1
)

if not exist benchmark\01-rand3sat-n060.cnf "%BIN%" gen-benchmark benchmark

if "%TCS_TIMEOUT%"=="" set TCS_TIMEOUT=120
"%BIN%" benchmark benchmark --no-ui --timeout %TCS_TIMEOUT% %*
