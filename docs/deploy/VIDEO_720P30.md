# 720p30 视频链路与连续性验收

## 2026-10-12 有线连接与 YOLO 结果交付

自动识别同时探测活动网口和 Wi-Fi。同一已记住设备同时回应时优先采用网口；只有无线回应时短暂等待 150ms 后采用无线。RTSP 目标处于本机活动 Ethernet 子网时，优先使用 UDP，建联失败回退 TCP；其他路径继续 TCP 优先。通过实际接口与子网判断，不写死设备 IP。USB/RNDIS 若被 Windows 归类为 Ethernet，也采用该策略，但本轮实测对象仅为直接网线。

YOLO 推理结束立即交付新结果，保持会话隔离、相同结果序号去重和 HOLD/未检出零偏移。20ms 发送限流通过精确定时器补发最新结果，避免等待下一张视频帧。时间叠加仍只在显示层绘制。

YOLO输入采用保持比例的640×640 letterbox（114补边），输出还原原图坐标。跟踪引擎直接采用DnnThread已平滑的完整框，避免再次平滑宽高而制造中心漂移。已通过公开bus.jpg与官方Ultralytics的真实模型对照及中心固定/尺寸变化回归。需要定位跟随时可在启动前设`QT_TRACK_CONTROL_TRACE=1`，日志只记录坐标、置信度、时间和遥测，不保存摄像头画面；未设置时关闭。局部检测与加速试验现场退步已撤回；最终恢复版本39项Qt通过，用户在手机静止和小幅左右移动复测后反馈“现在就很好”，正式EXE保留该版。未修改或烧录STM32固件；更大动作、不同朝向和长期连续性仍需独立验证。


实测对照中 TCP 有短时集中到达；有线 UDP 的 60 秒视频及 90 秒 YOLO26s 实流负载均为每秒 30–31 次不同画面绘制，90 秒最大绘制间隔 43.35ms、推理结果间隔 47.16ms。数据仅证明该次窗口绘制与推理节奏，未证明电机跟随或相机到屏幕延迟。

最终自动发现独立 120 秒 YOLO26s 测量：逐秒均 30 帧，最大绘制间隔 58.70ms（1次大于50ms），最大推理结果间隔 64.71ms；没有重连或摄像头错误。另一次存在并发主机工作时出现 107.80ms 绘制间隔，该失败记录保留。不能由逐秒计数宣称长期零抖动，详情见 [本次报告](../learning/2026-10-12-01-wired-video-cadence-and-yolo-dispatch.md)。

探针现在使用正常 Qt 事件循环测量，避免 qWait 轮询睡眠合并重绘。支持 `--auto-discover`，`--yolo-benchmark` 使用实时视频和正式推理代码并断开控制，保存时间与状态元数据，不保存图像。例：

```text
video_cadence_probe --auto-discover --seconds 120 --yolo-benchmark --output <忽略目录>/video.json
python tools/analyze_video_cadence.py <忽略目录>/video.json --output <忽略目录>/analysis.json
```

OpenCV 通路仍不提供源 PTS；分析器不会据此宣布源时间戳连续性验收完成。逐秒绘制统计和最长空档需分别报告。旧章节中的固定 TCP 优先说明对应历史版本。

## 2026-10-11 项目清理

当前视频仍使用下文的 OpenCV/TCP 优先链路。未接入正式构建的 `rtspdecoder.*`、`rtspstreamhealth.h`、`videoplayout.h` 及其专用单元测试已从工作树移除，需要研究时可从阶段提交 `38f1365135b4f073a19bd16bd4984a2d53f57265` 恢复。`tests/video_cadence_probe.*`、`tools/analyze_video_cadence.py` 及分析器回归测试仍用于当前正式链路，继续保留。后面的原生 FFmpeg 实验内容是历史记录；清理范围见 [项目清理报告](../learning/2026-10-11-09-project-cleanup.md)。

## 2026-10-11 连接失败修复

回退后的首次建联失败已定位到板端：`rkipc` 收到 SIGTERM 退出，554 无监听，控制服务仍在线。已经恢复视频，并部署 `S99zzzvideo` 统一原厂与 init 的视频启动入口、退出退避恢复、正常停止和小日志限制；视频配置保持原样。安装与维护方法见 [板端视频服务](../../board/luckfox/README.md#视频服务退出后的恢复)。

真实正式窗口收到 1280×720，连续 60 秒无媒体超时或控制掉线；独立真实设备 Qt 与正式可见窗口复测的关闭/刷新/重开流程均通过。一次可见窗口刷新检查出现额外开流，原始失败记录和后续核对结果均保留在 [修复报告](../learning/2026-10-11-06-rtsp-video-service-recovery.md)。本轮未重启整板，不能声称重新上电已验证；最初信号发送来源、长期逐秒连续性及光学端到端延迟仍未确认。

## 2026-10-11 视频链路回退

按用户要求，正式程序的视频收流/解码/显示恢复为 `ecd73bfdf824a2a131657a06e2c5444a96756238` 的方案：OpenCV FFmpeg 后端、TCP 优先（建联失败回退 UDP）、每轮一次 grab/retrieve、最新帧覆盖后直接显示；删除正式构建中的原生 FFmpeg 开发依赖和 150ms 定时播放队列。打开超时 5 秒、读取超时 2 秒、无帧巡检门槛 5 秒沿用该版本。

保留后来修复的取消线程回收和会话通知同步，保留现有无线控制、默认垂直翻转、跟踪方向及左上角时间。原生解码与播放实验源码仍保留，退出正式程序构建；此前的实验记录保留在下方。

本次源码与正式 EXE 备份：`tmp/video-rollback-ecd73bf-20261011-183216/`，包括修改前核心源码、测试、部署说明、Git 差异和 `untitled7-before.exe`。回退不更改板端编码、无线配置或 QoS。

当前构建仍使用 Qt 6.10.1 / MSVC 2022 / OpenCV 4.12 / ONNX Runtime 1.20.1；不需要 `FFMPEG_DIR`。在独立构建目录初始化 `vcvars64.bat`，运行 qmake/jom，沿用正式目录已部署的运行库和插件。

当前 `tests/video_cadence_probe.pro` 已适配直接显示通路，参数为 `--url`、`--seconds`、`--output`，传输策略固定为正式代码的 TCP 优先。探针记录收到的画面通知序号及 Qt 绘制事件，不保存图像。旧后端不提供源 PTS，JSON 将其标记为不可用，分析器仍输出绘制计数和空档，但不会将其判为源时间戳连续性验收通过。

构建、软件回归与实际部署结果见 [本次回退报告](../learning/2026-10-11-05-video-stream-rollback-ecd73bf.md)。恢复源码不代表恢复当时的无线环境，也不代表本次已测得连续 30 FPS 或光学端到端延迟。

## 以下为回退前原生解码实验记录

下面的数据通路、FFmpeg 开发依赖、`--delay-ms`/`--tcp` 探针参数和试验配置描述，记录回退前状态，当前正式程序已经退出这套通路。

## 当前状态

目标：原生 1280×720、每个源画面只显示一次、按 33.333 ms 的节奏播放，优先把画面延迟控制在 300 ms 内。

2026-10-11 已实现原生 FFmpeg 解码、源时间戳播放和独立最新帧跟踪；**实机连续稳定性尚未通过，端到端光学延迟尚未测量**。短时间显示 30 FPS 不代表完成验收。

## 数据通路

1. 板端摄像头/ISP → H.264 编码器 → RTSP `/live/0`。
2. `RtspDecoder` 在独立线程解复用/解码，保留 PTS、序号和关键帧标记；拒绝已标记损坏的画面，损坏后等待关键帧。
3. 显示进入 `VideoPlayout` 有界队列，默认 150 ms，根据源 PTS 和单调时钟生成 30 Hz 的播放期限。重复、迟到、缺失和溢出分别统计；不会用旧帧补足计数或在停顿后突发追帧。
4. 跟踪通过独立的最新帧通知进入后台工作线程。接收积压超过 200 ms 时不使用该帧驱动跟踪。这个积压是接收相对时间指标，不是相机到屏幕延迟。
5. Qt 绘制安装方向校正后的画面；时间和跟踪框只在显示层叠加。关闭和切流取消旧会话；异常清除旧画面、跟踪输入和 FPS。

Wi-Fi 丢失的源画面不能由播放器恢复。网络停顿超过缓冲预算时，必须如实显示缺帧；增加一秒缓冲不符合本次低延迟约束。

## 构建

本机：Qt 6.10.1 / MSVC 2022 / OpenCV 4.12 / ONNX Runtime 1.20.1。新增 FFmpeg 开发依赖：设置 `FFMPEG_DIR`，目录包含 `include/libavformat`、`include/libavcodec`、`include/libavutil`、`include/libswscale` 和对应导入库 `lib/`。

本机开发目录是 `D:/ffmpeg-sdk/7.1.3`，头文件取自 [FFmpeg 官方发布源代码](https://ffmpeg.org/releases/ffmpeg-7.1.3.tar.xz)，Windows 导入库依据现有 Qt FFmpeg DLL 导出生成。运行目录已有 `avformat-61.dll`、`avcodec-61.dll`、`avutil-59.dll`、`swscale-8.dll`；程序核对这四个组件的主版本。迁移机器时需要匹配的开发包和运行库；这些二进制依赖不提交到仓库。

在初始化 `vcvars64.bat` 的 MSVC 终端中：

```text
qmake untitled7.pro CONFIG+=release CONFIG-=debug_and_release
jom -j4
```

使用独立构建目录，保留现有工作和旧程序备份。Qt 插件、OpenCV、ONNX Runtime 和 CUDA 依赖沿用正式运行目录。

## 实机验收

`tests/video_cadence_probe.pro` 直接编译正式源码和正式窗口，包含控制连接，记录提交到窗口的帧和 `QEvent::Paint` 对应的源身份。只保存时序 JSON，不保存摄像头图像。

```text
qmake tests/video_cadence_probe.pro CONFIG+=release CONFIG-=debug_and_release
jom -j4
video_cadence_probe --url rtsp://<实时发现的板子地址>/live/0 --seconds 90 --delay-ms 150 --output <忽略目录>/video.json
python tools/analyze_video_cadence.py <忽略目录>/video.json --output <忽略目录>/analysis.json
```

TCP 对照添加 `--tcp`。不要同时打开多个媒体客户端；正常性能测试期间不要执行板端维护/采样连接，以免给小内存设备引入额外压力。测量前确认电脑和板子的当前网络、地址、服务与原生编码规格。

分析器检查每秒不同源画面、源 PTS 缺口、绘制间隔、首尾空档和测量期间重连。29/31 的单帧秒边界差异单独报告；源帧缺口、超过 50 ms 的绘制间隔、低于 20 ms 的追帧间隔和重连都会使连续性验收失败。先通过 90 秒，再测至少 10 分钟。不能用平均 FPS 通过验收。

`QEvent::Paint` 证明 Qt 绘制过窗口内容，不能证明显示器每次扫描的实际时间，也不能证明相机到屏幕低于 300 ms。光学延迟需要摄像头拍摄物理 LED 或同一时间基准，并同时记录真实屏幕；界面叠加的电脑时间不属于这个测量。

## 配置与回退

当前实板试验值：1280×720、源/输出 30 FPS、H.264 High、CBR 1024 kbps、GOP15，视频 AF41 保持关闭。该值改善了短窗口的画面连续性，但尚不是通过验收的最终配置。原配置备份位于板端 `/userdata/backup-stable-video-20261011/rkipc.ini`。调整后只重启摄像头服务并核对 PID、输入/输出和配置；先保存备份。

原正式 EXE 备份和时序数据位于被忽略的 `tmp/stable-video-20261011/`。恢复 EXE 前先关闭当前程序，再复制备份。不要把 Wi-Fi 凭据、SSH 私钥、控制密钥和私人画面加入报告或仓库。
