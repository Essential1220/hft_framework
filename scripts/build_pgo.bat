@echo off
setlocal

set CMAKE=C:\Users\Administrator\AppData\Local\Programs\Python\Python39\Scripts\cmake.exe
set CTP_SDK=D:\CTP_Project\sdk\v6.7.11_20250714_traderapi\v6.7.11_20250617_winApi\traderapi\20250617_traderapi64_se_windows
set PYBIND11_DIR=C:\Users\Administrator\AppData\Local\Programs\Python\Python39\lib\site-packages\pybind11\share\cmake\pybind11

if "%1"=="--use" goto phase2

echo === Phase 1: Instrument (PGO profile generation) ===
%CMAKE% -S . -B build_pgo -DPGO_GENERATE=ON -DCTP_SDK_DIR=%CTP_SDK% -Dpybind11_DIR=%PYBIND11_DIR%
if errorlevel 1 (echo CMake configure failed & exit /b 1)

%CMAKE% --build build_pgo --config Release
if errorlevel 1 (echo Build failed & exit /b 1)

echo.
echo === Phase 1 complete ===
echo Run build_pgo\Release\hft_framework.exe for 10-30 minutes under realistic load,
echo then Ctrl+C and re-run: build_pgo.bat --use
exit /b 0

:phase2
echo === Phase 2: Optimize (PGO profile use) ===
%CMAKE% -S . -B build_pgo -DPGO_GENERATE=OFF -DPGO_USE=ON -DCTP_SDK_DIR=%CTP_SDK% -Dpybind11_DIR=%PYBIND11_DIR%
if errorlevel 1 (echo CMake configure failed & exit /b 1)

%CMAKE% --build build_pgo --config Release
if errorlevel 1 (echo Build failed & exit /b 1)

echo === PGO-optimized build complete ===
