@echo off
rem Ghidra's batch launcher breaks on paths with "(x86)", so run it from a subst drive.
rem Usage: run_headless.cmd <drive letter mapped to rip3-coop> [script]
rem With a script name, reruns only that script on the already analyzed project.
setlocal
set "D=%~1:"
if not "%~2"=="" (
  set "JAVA_HOME=%D%\tools\jdk\jdk-21.0.12.1+1"
  set "PATH=%D%\tools\jdk\jdk-21.0.12.1+1\bin;%SystemRoot%\System32;%SystemRoot%"
  cd /d %D%\
  call %D%\tools\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat %D%\local\ghidra_proj rip3 -process RIP3_dump.exe -noanalysis -scriptPath %D%\scripts\ghidra -postScript %~2 %D%\local\dump\iat_map.csv %D%\local\re > %D%\local\ghidra.log 2>&1
  exit /b
)
set "JAVA_HOME=%D%\tools\jdk\jdk-21.0.12.1+1"
set "PATH=%JAVA_HOME%\bin;%SystemRoot%\System32;%SystemRoot%"
cd /d %D%\
call %D%\tools\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat %D%\local\ghidra_proj rip3 -import %D%\local\dump\RIP3_dump.exe -overwrite -scriptPath %D%\scripts\ghidra -postScript ExportNetCode.java %D%\local\dump\iat_map.csv %D%\local\re > %D%\local\ghidra.log 2>&1
exit /b %ERRORLEVEL%
