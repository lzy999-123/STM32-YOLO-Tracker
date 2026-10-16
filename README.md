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

论文方法实施的第一阶段已提供默认关闭的旁路日志、离线完整性检查、候选相机标定与静止目标近似 LOS 工具，见 [阶段一数据与标定说明](docs/research/STAGE1_DATA_AND_CALIBRATION.md)。当前缺少源采样时间和实物标定证据，工具不声明同步验证或抗遮挡性能通过；正式程序不自动替换。

每次提交都同步保存一份学习报告，记录完成的工作、学到的知识、故障与解决过程、验证结果和以后需要注意的事项。
报告索引见 [docs/learning](docs/learning/README.md)，首份报告覆盖Luckfox无线视频与自动识别阶段。
项目全部完成后，依据历次报告填写 [项目最终总结](docs/learning/PROJECT_SUMMARY.md)。持续维护约定见 [AGENTS.md](AGENTS.md)。

## 部署说明

当前阶段进展和验证范围见 [2026-10-11 阶段提交报告](docs/learning/2026-10-11-08-github-checkpoint.md)，当前视频链路见 [720p 视频部署与验收](docs/deploy/VIDEO_720P30.md)。较早的交接、驱动和阶段记录已归入 [历史文档](docs/archive/README.md)。

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

## 项目目录与维护

- 根目录保留正式 Qt 源码、`untitled7.pro`、协议和无线控制说明。
- `board/luckfox/`：当前板端服务、部署及驱动维护源码。
- `board/stm32/`：OLED Wi-Fi 页面、PC13 短长按说明和针对正式下位机基线的固件补丁。
- `tests/`：网络控制、跟踪/界面回归与当前视频节奏检查。
- `tools/`：YOLO 模型转换示例与当前视频节奏分析。
- `docs/deploy/`：部署及验收说明；`docs/learning/`：学习记录；`docs/archive/`：历史交接材料。
- `build/`：Qt Creator 的正式 Debug/Release 构建目录；`tmp/`：本地诊断证据、SSH 维护资料和驱动构建工具，均不进入仓库。

2026-10-11 已清理旧构建快照、未接入正式程序的 FFmpeg 播放实验及过时的 ONNX 1.18 下载脚本。当前 `.pro` 使用 ONNX Runtime 1.20.1；模型转换所需 `.pt` 和程序使用的 `.onnx` 继续保留。清理范围与恢复方法见 [清理报告](docs/learning/2026-10-11-09-project-cleanup.md)。

下位机 OLED 已新增当前 Wi-Fi 名称页，PC13 短按切页、长按 1 秒切换自动/手动；长名称按字形滚动。Luckfox 配套脚本已部署，用户确认短按后当前名称显示正确；长按和长名称滚动的现场验收尚未执行。使用与协议说明见 [STM32 OLED Wi-Fi 页面](board/stm32/README.md)，实板证据见 [部署记录](docs/learning/2026-10-11-11-oled-wifi-deployment.md)。
