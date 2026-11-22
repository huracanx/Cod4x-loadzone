@echo off
setlocal
where cl >nul 2>&1 || (echo Run from an x86 Native Tools Command Prompt for VS 2022 & exit /b 1)
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /LD /Ithird_party\detours src\plugin.cpp third_party\detours\detours.lib /link /OUT:build\loadzone.asi
if errorlevel 1 exit /b 1
