# STM32 YOLO26 Tracker

## 项目简介 (Project Overview)
本项目是一个基于 Qt C++ 开发的机器视觉追踪与控制系统。结合了 YOLO 深度学习目标检测算法和目标追踪机制，通过分析摄像头捕捉的画面识别特定目标，并计算其位置信息。最终通过串口与 STM32 微控制器进行通信，以实现云台或相关硬件的实时自动追踪控制。

## 核心功能 (Core Features)
*   **图像采集 (Camera Manager)**：支持本地摄像头或视频流的实时画面获取。
*   **目标检测与追踪 (DNN & Tracking Engine)**：
    *   内置基于 YOLO26 模型的深度学习推理模块 (`dnnthread.cpp`)，实现高精度目标检测。
    *   目标追踪模块 (`trackingengine.cpp`) 对连续帧中的目标进行锁定跟踪，计算偏移量。
*   **硬件通信 (Serial Controller)**：通过串口(`serialcontroller.cpp`)将目标的相对坐标和状态指令实时发送给下位机（STM32）。
*   **可视化操作界面 (Qt GUI)**：友好的用户界面 (`mainwindow.cpp`)，实时展示画面检测结果（画框标注）以及设备通信状态。

## 代码结构说明
*   `mainwindow.cpp/h`：主程序界面逻辑及各模块的统筹调度。
*   `cameramanager.cpp/h`：摄像头硬件管理与图像获取。
*   `dnnthread.cpp/h`：负责图像的预处理、推理和后处理（独立线程运行以防止阻塞UI）。
*   `trackingengine.cpp/h`：实现多目标追踪及框选逻辑。
*   `serialcontroller.cpp/h`：串口通信封装模块。

## 部署说明

最新阶段进展和验证范围见 [2026-10-08 阶段记录](MILESTONE_2026-10-08.md)。

### Luckfox 无线摄像头

电脑连接与 Luckfox 相同的热点，保持默认的“Luckfox 自动识别”，点击“打开摄像头”即可。
上位机会自动发现板子的当前地址并打开 `/live/0`，无需填写 IP；查找时可点击“取消查找”。
板子刚上电需要约一分钟完成 Wi-Fi 连接。网线和 USB 直连仍可从下拉框选择。
设备发现由板子 `/etc/init.d/S99zzzdiscovery` 自动启动，使用 UDP 39093。
板子部署和驱动修复脚本见 [board/luckfox](board/luckfox/README.md)。

1.  环境准备：需安装 Qt 并在 `.pro` 中配置好 OpenCV 及 SerialPort。
2.  模型文件：由于体积较大，YOLO26 的 `.onnx` / `.pt` 模型文件不直接包含在代码库中，请根据需要自行放置到项目对应目录。默认文件名为 `yolo26s.onnx` / `yolo26n.onnx`。
3.  编译运行：通过 Qt Creator 打开 `.pro` 工程文件，构建并运行。
