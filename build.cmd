@echo off
rem Builds build\bink2w64.dll (sowguard), build\mutant\bink2w64.dll (negative control)
rem and build\test_guard.exe with the Visual Studio 2022 x64 tools.
rem Set VSINSTALL if Visual Studio is not in the default place.
setlocal
cd /d "%~dp0"
if "%VSINSTALL%"=="" set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || goto :fail
if not exist build\mutant mkdir build\mutant
if not exist build\fake mkdir build\fake
set CF=/nologo /O2 /MT /EHsc /W4 /WX /std:c++17 /DUNICODE /D_UNICODE
set SRC=src\sowguard.cpp src\probe.cpp src\steamguard.cpp src\input.cpp src\coop.cpp src\simhook.cpp
cl %CF% /LD %SRC% /Fobuild\ /Fdbuild\ /Febuild\bink2w64.dll || goto :fail
cl %CF% /DSOWGUARD_MUTANT /LD %SRC% /Fobuild\mutant\ /Fdbuild\mutant\ /Febuild\mutant\bink2w64.dll || goto :fail
rem Test-only stand-in for Steam's steam_api64.dll (never copied into a game folder).
cl %CF% /LD src\fake_steam_api.cpp /Fobuild\fake\ /Fdbuild\fake\ /Febuild\fake\steam_api64.dll || goto :fail
cl %CF% src\test_guard.cpp /Fobuild\ /Fdbuild\ /Febuild\test_guard.exe || goto :fail
rem The transport's simulator tests and their negative control (reliable messages unordered: must fail).
cl %CF% src\net_test.cpp /Fobuild\ /Fdbuild\ /Febuild\net_test.exe || goto :fail
cl %CF% /DSOWNET_MUTANT src\net_test.cpp /Fobuild\mutant\ /Fdbuild\mutant\ /Febuild\mutant\net_test.exe || goto :fail
rem The enemy messages (host tracker, client tracker) without any game code.
cl %CF% src\enemy_test.cpp /Fobuild\ /Fdbuild\ /Febuild\enemy_test.exe || goto :fail
echo BUILD_OK
exit /b 0
:fail
echo BUILD_FAILED
exit /b 1
