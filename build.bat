IF NOT EXIST "vcpkg_installed/x64-windows-static-md/lib/libcurl.lib" (
  echo ERROR libcurl not found
  echo niraclient-c uses libcurl, and has been tested with libcurl 7.86.0 compiled statically.
  echo
  echo If you'd like to build libcurl yourself, it's pretty easy -- just run the "vcpkg install" command below
  echo from within this directory. It's important to run it from this directory so it uses the included
  echo vcpkg.json file:
  echo
  echo vcpkg install --triplet=x64-windows-static-md
  echo
  echo After the command finishes, you should end up with an vcpkg_installed directory containing the
  echo subfolders x64-windows and x64-windows-static-md.
  echo
  echo If you'd prefer not to build libcurl, you can download the pre-built libcurl-static-win64.zip artifact from
  echo the niraclient-c repository and extract its contents into the niraclient-c directory.
  echo For example, go to this page and download libcurl-static-win64.zip near the bottom:
  echo https://github.com/NiraOfficial/niraclient-c/actions/runs/3946065893
  echo After extraction, you should end up with an vcpkg_installed directory containing the
  echo subfolders x64-windows and x64-windows-static-md. Note, this was built using VS2019,
  echo so your mileage may vary if you're using an older VS. If in doubt, it's best to build using vcpkg.
  goto :EOF
)

IF EXIST "C:/Program Files (x86)/Microsoft Visual Studio/2019/Enterprise/VC/Auxiliary/Build/vcvarsall.bat" (
  call "C:/Program Files (x86)/Microsoft Visual Studio/2019/Enterprise/VC/Auxiliary/Build/vcvarsall.bat" amd64
)

where /q cl.exe
if ERRORLEVEL 1 (
  echo ERROR: Could not find cl.exe. This script must be run from within a Visual Studio Command prompt.
  goto :EOF
)

set CURL_ROOT=./vcpkg_installed/x64-windows-static-md

cl.exe /MD /arch:AVX /openmp /O2 /DCURL_STATICLIB=1 /Oi /I%CURL_ROOT%/include /I. niraclient.c example_usage.c %CURL_ROOT%/lib/libcurl.lib %CURL_ROOT%/lib/zlib.lib ws2_32.lib wldap32.lib advapi32.lib kernel32.lib comdlg32.lib crypt32.lib normaliz.lib
