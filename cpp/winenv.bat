@echo off
rem Set up MSVC (vcvars64) + Rust (cargo) on PATH.
rem Usage: call winenv.bat, then run cmake / ninja / cargo.
rem NOTE: put the vswhere path into a var first. Writing %ProgramFiles(x86)%
rem       directly inside for(...) breaks: the ) in (x86) collides with for's ).
rem NOTE: keep this file pure ASCII. cmd reads .bat in the OEM codepage, so
rem       non-ASCII comments get executed as commands and error out.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo winenv: Visual Studio not found 1>&2
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%USERPROFILE%\.cargo\bin;%PATH%"