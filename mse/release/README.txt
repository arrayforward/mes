# MES 车间世界模型 —— 发布包

## 内容

- `mse_server.exe` —— MES 验证服务器(HTTP API + H5 界面托管,自带 sqlite 持久化)
- `mse_demo.exe` —— 演示场景客户端("总装车间的一天",打正在运行的服务器)
- `start_mes.bat` —— 启动服务器并打开浏览器(默认 http://127.0.0.1:18080/)
- `run_demo.bat` —— 向服务器灌入演示数据(可选)
- `stop_mes.bat` —— 停止服务器
- `web/` —— H5 界面(React,免构建,服务器托管)
- `assets/` —— 种子资产(制造域本体/词典/WASM 规则产物)

## 快速开始

1. 双击 `start_mes.bat`(浏览器自动打开验证台);
2. 可选:双击 `run_demo.bat` 灌入演示场景;
3. 数据落盘在同目录 `mse_server.db` / `mse_server_voxels.db`,删掉即回到出厂状态。

命令行用法:`mse_server.exe [--host 127.0.0.1] [--port 18080] [--db ...] [--seed]`
