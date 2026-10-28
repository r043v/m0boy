@echo off
python -m pip install --quiet pyserial
start "" pythonw "%~dp0serial_console.pyw"
