@echo off
setlocal
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /O2 /MT /D WIN32_LEAN_AND_MEAN ^
   /I third_party\detours ^
   /I third_party\imgui ^
   /I third_party\imgui\backends ^
   src\plugin.cpp src\zone_menu.cpp ^
   third_party\imgui\imgui.cpp ^
   third_party\imgui\imgui_draw.cpp ^
   third_party\imgui\imgui_tables.cpp ^
   third_party\imgui\imgui_widgets.cpp ^
   third_party\imgui\backends\imgui_impl_win32.cpp ^
   third_party\imgui\backends\imgui_impl_dx9.cpp ^
   /link /DLL /OUT:build\loadzone.asi ^
   third_party\detours\detours.lib ^
   d3d9.lib user32.lib gdi32.lib advapi32.lib
if errorlevel 1 exit /b 1
echo Built build\loadzone.asi
endlocal