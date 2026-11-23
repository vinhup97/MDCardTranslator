@echo off
rem Build MDCardTranslator -> out\version.dll (x64). Run: build.bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
if not exist out mkdir out
cl /nologo /LD /O2 /MT /EHsc /utf-8 dllmain.cpp minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde64.c /I minhook\include /Fo:out\ /Fe:out\version.dll
