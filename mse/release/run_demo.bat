@echo off
chcp 65001 >nul
cd /d %~dp0
echo 以 HTTP 客户端方式向 127.0.0.1:18080 灌入"总装车间的一天"演示场景...
echo (请先运行 start_mes.bat 启动服务器)
mse_demo.exe --server 127.0.0.1:18080
pause
