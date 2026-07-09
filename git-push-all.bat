@echo off
setlocal

cd /d "%~dp0"

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo [ERROR] BAT-файл запущен не внутри Git-репозитория.
    pause
    exit /b 1
)

git remote get-url origin >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Remote "origin" не настроен.
    pause
    exit /b 1
)

echo Pushing all branches...
git push origin --all
if errorlevel 1 goto :error

echo Pushing all tags...
git push origin --tags
if errorlevel 1 goto :error

echo.
echo [OK] Все локальные ветки и теги отправлены без удаления удалённых refs.
pause
exit /b 0

:error
echo.
echo [ERROR] Push завершился с ошибкой.
pause
exit /b 1
