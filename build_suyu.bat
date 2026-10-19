@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set PATH=C:\glslang\bin;%PATH%

echo === Configuring CMake ===
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=externals/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DENABLE_QT=ON ^
  -DYUZU_USE_BUNDLED_QT=ON ^
  -DYUZU_USE_BUNDLED_FFMPEG=ON ^
  -DYUZU_USE_BUNDLED_SDL3=ON ^
  -DYUZU_USE_BUNDLED_OPENSSL=ON ^
  -DENABLE_WEB_SERVICE=OFF ^
  -DYUZU_TESTS=OFF

if %ERRORLEVEL% neq 0 (
  echo === CMake configuration FAILED ===
  exit /b %ERRORLEVEL%
)

echo === Building (Release) ===
cmake --build build --config Release -j%NUMBER_OF_PROCESSORS%

if %ERRORLEVEL% neq 0 (
  echo === Build FAILED ===
  exit /b %ERRORLEVEL%
)

echo === Build SUCCEEDED ===
echo Output binaries are in: build\bin\
dir /b build\bin\*.exe 2>nul
