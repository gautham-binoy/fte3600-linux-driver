@set L=C:\Windows\temp\install.log
@set IP=[********Install Pass********]
@set IPR=[********Install Pass, Restart is need********]
@set IF=[********Install Fail********]

@echo [%time%]=========Install Focaltech Fingerprint driver========= >> %L%

pnputil /add-driver "%~dp0Drivers\*.inf" /install >> %L%
@if %errorlevel%==0 (echo [%time%]%IP% >> %L% & goto resultpass)
@if %errorlevel%==3010 (echo [%time%]%IPR% >> %L% & goto resultpass)
@if not %errorlevel%==0 goto ERR

:resultpass
@echo [%time%]=========Intel Focaltech Fingerprint Install Complete========= >> %L%
ECHO ERRRORLEVEL=%ERRORLEVEL% >> %L%
@set LL=C:\Windows\temp\FocalFP.log
@echo [%time%] >>%LL%
EXIT /B 0

:ERR
echo [%time%]%IF% >> %L%
ECHO ERRRORLEVEL=%ERRORLEVEL% >> %L%
EXIT /B 1