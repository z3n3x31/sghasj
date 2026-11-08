@echo off
REM Necesita Visual Studio (Desktop development with C++), CMake si Git.
if not exist reshade git clone --depth 1 https://github.com/crosire/reshade reshade
git -C reshade submodule update --init --depth 1 deps/imgui
cmake -S . -B build -A x64 -DRESHADE_DIR="%CD%\reshade" || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo Gata: build\Release\ProShadeLite.addon64
