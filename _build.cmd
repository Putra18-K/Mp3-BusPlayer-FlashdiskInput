@echo off
cd /d "%~dp0"
echo IDF_PATH=%IDF_PATH% > env_check.txt
echo IDF_TOOLS_PATH=%IDF_TOOLS_PATH% >> env_check.txt
"C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe" "C:\esp\v6.0.1\esp-idf\tools\idf.py" fullclean >> build.log 2>&1
"C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe" "C:\esp\v6.0.1\esp-idf\tools\idf.py" build >> build.log 2>&1
echo IDF_EXIT=%ERRORLEVEL% >> build.log
