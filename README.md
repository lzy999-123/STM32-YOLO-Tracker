# STM32 YOLO26 Tracker

## 项目简介 (Project Overview)
本项目是一个基于 Qt C++ 开发的机器视觉追踪与控制系统。电脑通过 Wi-Fi 接收 Luckfox 视频，在电脑上识别、跟踪目标，再通过网络发送目标偏移和控制指令。Luckfox 通过设备内部 UART 与 STM32 通信，回传角度、模式和命令确认。电脑端不使用串口。

## 核心功能 (Core Features)
*   **图像采集 (Camera Manager)**：支持本地摄像头或视频流的实时画面获取。
*   **目标检测与追踪 (DNN & Tracking Engine)**：
    *   内置基于 YOLO26 模型的深度学习推理模块 (`dnnthread.cpp`)，实现高精度目标检测。
    *   目标追踪模块 (`trackingengine.cpp`) 对连续帧中的目标进行锁定跟踪，计算偏移量。
*   **无线控制 (Network Controller)**：`networkcontroller.cpp` 使用 UDP 控制通道；板端 `luckfox-control.py` 转发到 STM32 UART，确认信息沿原路返回。
*   **可视化操作界面 (Qt GUI)**：友好的用户界面 (`mainwindow.cpp`)，实时展示画面检测结果（画框标注）以及设备通信状态。

## 代码结构说明
*   `mainwindow.cpp/h`：主程序界面逻辑及各模块的统筹调度。
*   `cameramanager.cpp/h`：摄像头硬件管理与图像获取。
*   `dnnthread.cpp/h`：负责图像的预处理、推理和后处理（独立线程运行以防止阻塞UI）。
*   `trackingengine.cpp/h`：实现多目标追踪及框选逻辑。
*   `networkcontroller.cpp/h`：会话、网络心跳、命令确认与重发、遥测超时处理。
*   `hostwifimonitor.cpp/h`：后台读取 Windows 当前 Wi-Fi 名称。
*   `board/luckfox/luckfox-control.py`：板端网络与 UART 的双向转发。

## 学习记录与项目总结

每次提交都同步保存一份学习报告，记录完成的工作、学到的知识、故障与解决过程、验证结果和以后需要注意的事项。
报告索引见 [docs/learning](docs/learning/README.md)，首份报告覆盖Luckfox无线视频与自动识别阶段。
项目全部完成后，依据历次报告填写 [项目最终总结](docs/learning/PROJECT_SUMMARY.md)。持续维护约定见 [AGENTS.md](AGENTS.md)。

## 部署说明

最新阶段进展和验证范围见 [2026-10-08 阶段记录](MILESTONE_2026-10-08.md)。

### Luckfox 无线摄像头

电脑连接与 Luckfox 相同的热点，保持默认的“Luckfox 自动识别”。程序启动后自动发现设备并连接控制通道；点击“打开摄像头”才显示视频。关闭摄像头后控制连接继续保留。“刷新”重新读取 Wi-Fi 并查找设备，显示刷新进度，不会在视频关闭时自动打开视频。
上位机会自动发现板子的当前地址并打开 `/live/0`，无需填写 IP；查找时可点击“取消查找”。
板子刚上电需要约一分钟完成 Wi-Fi 连接。网线和 USB 直连仍可从下拉框选择。
设备发现由板子 `/etc/init.d/S99zzzdiscovery` 自动启动，使用 UDP 39093。
板子部署和驱动修复脚本见 [board/luckfox](board/luckfox/README.md)。

无线控制的部署、验收范围及当前实机限制见 [无线控制说明](WIRELESS_CONTROL.md)。视频连通与 STM32 云台在线分别显示，不能用视频状态代替云台状态。

1.  环境准备：需安装 Qt Widgets、Network、Concurrent、Multimedia，并在 `.pro` 中配置 OpenCV 和 ONNX Runtime。电脑端不依赖 Qt SerialPort。
2.  模型文件：由于体积较大，YOLO26 的 `.onnx` / `.pt` 模型文件不直接包含在代码库中，请根据需要自行放置到项目对应目录。默认文件名为 `yolo26s.onnx` / `yolo26n.onnx`。
3.  编译运行：通过 Qt Creator 打开 `.pro` 工程文件，构建并运行。
