@echo off
chcp 65001 >nul
cd /d %~dp0
title mse_server - 车间世界模型 MES
echo 启动 MES 验证服务器(默认 127.0.0.1:18080,空库自动播种)...
start "" http://127.0.0.1:18080/
mse_server.exe
pause
