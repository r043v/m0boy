@echo off
rem Double-clic : ouvre l'interface de conversion de ROMs pour la Gamebuino AKA.
where pyw >nul 2>&1 && ( start "" pyw "%~dp0convert_gui.pyw" & exit /b 0 )
where pythonw >nul 2>&1 && ( start "" pythonw "%~dp0convert_gui.pyw" & exit /b 0 )
echo Python 3 est requis (https://www.python.org/downloads/, cocher "Add to PATH").
pause
