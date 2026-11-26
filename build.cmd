@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem Copyright (C) 2026 out_wasapi contributors. See LICENSE.
setlocal
where cmake >nul 2>nul
if errorlevel 1 if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake 3.21 or newer must be installed and available on PATH.
  exit /b 1
)
set "ROOT=%~dp0"
rem CMake chooses an installed Visual Studio generator; Win32 is mandatory.
cmake -S "%ROOT%." -B "%ROOT%build" -A Win32
if errorlevel 1 exit /b 1
cmake --build "%ROOT%build" --config Release --parallel
if errorlevel 1 exit /b 1
ctest --test-dir "%ROOT%build" -C Release --output-on-failure
exit /b %errorlevel%
