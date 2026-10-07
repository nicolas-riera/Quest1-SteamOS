@echo off
setlocal EnableExtensions
title Quest1-SteamOS
rem Boots an Oculus Quest 1 (unlocked bootloader) into native SteamOS, once.
rem Only "fastboot boot": the image goes to RAM, nothing is written to the headset.
rem Rebooting the headset brings back its normal system.
set "HERE=%~dp0"
set "IMG=%HERE%boot-native-holo.img"
set "T=%TEMP%\quest1-steamos-%RANDOM%.txt"
rem the Windows tools, even when Git or MSYS put their own find first in PATH
set "FIND=%SystemRoot%\System32\find.exe"
set "FINDSTR=%SystemRoot%\System32\findstr.exe"
cd /d "%HERE%"

echo.
echo  Quest1-SteamOS - boot native SteamOS on an Oculus Quest 1
echo  Nothing is flashed: the headset goes back to its normal system on the next reboot.
echo.
if not exist "%IMG%" (
	echo [!] boot-native-holo.img must be in the same folder as this script.
	goto :end
)
call :tools || goto :end

:detect
call :count_fastboot
if %NFB% GTR 1 (
	echo [!] More than one device is in fastboot mode: leave only the Quest plugged in.
	goto :end
)
if %NFB% EQU 1 goto :check

"%ADB%" devices > "%T%" 2>nul
"%FINDSTR%" /c:"unauthorized" "%T%" >nul && (
	echo [!] Put the headset on and allow USB debugging for this computer, then press a key.
	pause >nul
	goto :detect
)
set NADB=0
for /f %%c in ('type "%T%" ^| "%FINDSTR%" /e /c:"device" ^| "%FIND%" /c /v ""') do set NADB=%%c
if %NADB% GTR 1 (
	echo [!] More than one Android device is connected: leave only the Quest plugged in.
	goto :end
)
if %NADB% EQU 1 (
	"%ADB%" shell getprop ro.product.device > "%T%" 2>nul
	"%FINDSTR%" /b /c:"monterey" "%T%" >nul || (
		echo [!] The connected device is not an Oculus Quest 1.
		goto :end
	)
	echo Restarting the headset into its bootloader...
	"%ADB%" reboot bootloader
	set WATCHADB=0
	goto :wait
)
echo No headset found. Either:
echo  - plug it in with Android/Horizon OS running and developer mode + USB debugging on, or
echo  - turn it off, then hold Volume - and press Power until the boot menu appears.
echo Waiting for the headset (Ctrl+C to quit)...
set WATCHADB=1

:wait
for /l %%i in (1,1,60) do (
	call :count_fastboot
	call :gotfastboot && goto :check
	if %WATCHADB% EQU 1 call :adbready && goto :detect
	ping -n 3 127.0.0.1 >nul
)
goto :wait

:check
"%FB%" getvar product > "%T%" 2>&1
"%FINDSTR%" /c:"product: monterey" "%T%" >nul || (
	echo [!] The device in fastboot mode is not an Oculus Quest 1:
	type "%T%"
	goto :end
)
"%FB%" getvar unlocked > "%T%" 2>&1
"%FINDSTR%" /c:"unlocked: yes" "%T%" >nul || (
	echo [!] The bootloader of this headset is locked: it cannot boot this image.
	goto :end
)
echo Oculus Quest 1 with an unlocked bootloader found.
echo Press a key to start native SteamOS (Ctrl+C to cancel).
pause >nul
"%FB%" boot "%IMG%"
if errorlevel 1 (
	echo [!] fastboot boot failed.
	goto :end
)
echo.
echo Done. The headset is starting native SteamOS (about one minute).
echo  - Over USB, the headset is 192.168.77.1: ssh root@192.168.77.1
echo    (or telnet 192.168.77.1 for the rescue shell).
echo  - SteamOS itself lives in /data/steamos/holo.img on the headset (see the README):
echo    without it, the headset stays in the rescue shell.
echo  - Back to the normal system: reboot the headset (hold Power for 10 seconds).

:end
del "%T%" 2>nul
echo.
pause
exit /b

rem --- helpers -------------------------------------------------------------------------------------
:count_fastboot
set NFB=0
"%FB%" devices > "%T%" 2>nul
for /f %%c in ('type "%T%" ^| "%FIND%" /c "fastboot"') do set NFB=%%c
exit /b 0

:gotfastboot
if %NFB% GEQ 1 exit /b 0
exit /b 1

:adbready
"%ADB%" devices > "%T%" 2>nul
"%FINDSTR%" /e /c:"device" "%T%" >nul
exit /b %errorlevel%

:tools
if exist "%HERE%platform-tools\fastboot.exe" (
	set "FB=%HERE%platform-tools\fastboot.exe"
	set "ADB=%HERE%platform-tools\adb.exe"
	exit /b 0
)
where fastboot >nul 2>nul && where adb >nul 2>nul && (
	set "FB=fastboot"
	set "ADB=adb"
	exit /b 0
)
echo fastboot and adb were not found on this computer.
choice /c YN /m "Download Google's Android platform-tools next to this script"
if errorlevel 2 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile 'platform-tools.zip'; Expand-Archive -Force 'platform-tools.zip' '.'; Remove-Item 'platform-tools.zip'"
if not exist "%HERE%platform-tools\fastboot.exe" (
	echo [!] The download failed.
	exit /b 1
)
set "FB=%HERE%platform-tools\fastboot.exe"
set "ADB=%HERE%platform-tools\adb.exe"
exit /b 0
