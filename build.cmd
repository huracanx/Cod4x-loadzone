@echo off
setlocal
if not exist build mkdir build
cl /nologo /EHsc /O2 /MT /D WIN32_LEAN_AND_MEAN /I third_party\detours\include ^
   src\plugin.cpp src\zone_menu.cpp ^
   /link /DLL /OUT:build\loadzone.asi ^
   third_party\detours\lib\x86\detours.lib user32.lib gdi32.lib advapi32.lib
if errorlevel 1 exit /b 1
echo Built build\loadzone.asi
endlocal