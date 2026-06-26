@echo off
REM Thin wrapper for the documented CMake configure + build (Windows).
setlocal
if not exist build mkdir build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TELEMETRY=ON -DENABLE_EAC=ON || exit /b 1
cmake --build build --config Debug --parallel || exit /b 1
endlocal
