@echo off
rem Sign cs2_128tick.exe with the Casio certificate (Windows SDK signtool).
rem The .pfx is PRIVATE - keep it outside this repo. Usage:
rem   set CASIO_PFX=C:\keys\casio.pfx
rem   set CASIO_PFX_PASS=yourpassword
rem   sign.bat
if "%CASIO_PFX%"=="" (echo Set CASIO_PFX and CASIO_PFX_PASS first & exit /b 1)
signtool sign /f "%CASIO_PFX%" /p "%CASIO_PFX_PASS%" /fd sha256 /tr http://timestamp.digicert.com /td sha256 /d "CS2 128-Tick Patcher" "%~dp0cs2_128tick.exe" || exit /b 1
signtool verify /pa /v "%~dp0cs2_128tick.exe"
