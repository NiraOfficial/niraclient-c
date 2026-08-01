@echo off
setlocal

IF NOT EXIST "./vcpkg_installed/x64-windows-static-md/lib/libcurl.lib" (
  echo ERROR libcurl not found
  echo niraclient-c uses libcurl, and has been tested with libcurl 8.18.0 compiled statically.
  echo
  echo To build libcurl, run the "vcpkg install" command below from within this directory.
  echo vcpkg install --triplet=x64-windows-static-md
  echo
  echo It's important to run it from this directory so it uses the included vcpkg.json file.
  echo
  echo After the command finishes, you should end up with an vcpkg_installed directory containing the
  echo subfolders x64-windows and x64-windows-static-md.
  echo
  echo Zstandard:
  echo niraclient-c uses the BSD licensed libzstd. Zstandard v1.5.7 is included as a single source code file
  echo in zstd/zstd.c, so it is not necessary to download or install it.
  echo
  echo To link to your own copy of the libzstd library, remove /DNIRACLIENT_USE_INCLUDED_ZSTD_C from the
  echo build command below and update the build command to point to your zstd include and library file paths.
  echo
  echo If you wish to generate your own zstd.c file from the zstd source code rather than using the one provided,
  echo you can run the following commands from a bash prompt:
  echo git clone https://github.com/facebook/zstd
  echo git checkout v1.5.7 # Optional, but this is the version we're using at the time of this writing.
  echo cd zstd/build/single_file_libs
  echo ./create_single_file_library.sh
  echo
  echo Then copy the resulting zstd.c file into the zstd directory of this repository, being sure to replace
  echo the existing one. Be sure to keep /DNIRACLIENT_USE_INCLUDED_ZSTD_C in your build command, as well.
  echo
  goto :EOF
)

where /q cl.exe
if NOT ERRORLEVEL 1 goto :have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLPATH=%%i"
if "%VSINSTALLPATH%"=="" (
    echo ERROR: Could not locate a Visual Studio installation with the C++ toolset.
    exit /b 1
)
call "%VSINSTALLPATH%\VC\Auxiliary\Build\vcvars64.bat" amd64
:have_cl

set "TRIPLET=%~1"
if "%TRIPLET%"=="" set "TRIPLET=x64-windows-static-md"

set VCPKG_PATH=.\vcpkg_installed\%TRIPLET%

set "ZLIB_LIB=%VCPKG_PATH%\lib\zlib.lib"
if not exist "%ZLIB_LIB%" set "ZLIB_LIB=%VCPKG_PATH%\lib\zlibstatic.lib"
if not exist "%ZLIB_LIB%" set "ZLIB_LIB=%VCPKG_PATH%\lib\zs.lib"
if not exist "%ZLIB_LIB%" (
    echo ERROR: Could not find zlib.lib, zlibstatic.lib, or zs.lib in %VCPKG_PATH%\lib
    dir "%VCPKG_PATH%\lib"
    exit /b 1
)

cl.exe /MD /arch:AVX /openmp /O2 /Oi ^
    /DCURL_STATICLIB ^
    /DNIRACLIENT_USE_INCLUDED_ZSTD_C ^
    /I"%VCPKG_PATH%\include" /I. ^
    niraclient.c example_usage.c ^
    "%VCPKG_PATH%\lib\libcurl.lib" ^
    "%ZLIB_LIB%" ^
    ws2_32.lib advapi32.lib crypt32.lib wldap32.lib normaliz.lib ^
    iphlpapi.lib secur32.lib

endlocal
