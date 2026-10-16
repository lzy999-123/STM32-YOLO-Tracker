# 论文阶段一：时间戳日志、相机标定与静止目标检查

本阶段仅新增旁路数据记录和离线工具，未接入 LOS/Kalman 角度闭环，未改变控制方向、增益、20 ms 发送限流或 HOLD 零偏移。不要将源码构建通过写成实机跟踪通过。当前正式 EXE 不被本阶段自动替换。

## 1. 已实现与仍缺失

| 项目 | 当前能力 | 不能据此宣称 |
| --- | --- | --- |
| 帧身份 | 同一缓存锁下传递帧编号、流会话、接收时刻与图像方向；最新帧覆盖不复用旧身份 | 收到的帧就是曝光时刻的实时场景 |
| 检测记录 | YOLO 更新记录原始检测框、类别、置信度、DNN 会话和主机处理时间；HOLD/miss 不作新测量 | 同类别或同会话等于物理身份已正确标注 |
| 初始化 | 独立 inference_initialization 事件，不捏造初始化图像的采样时刻 | 初始化框拥有完整帧来源；CSRT 结果已接入此研究格式 |
| 遥测 | 接收时间、连接代号、序号、角度、模式、故障与校准有效标志 | 编码器采样时间等于主机接收时间 |
| 控制 | 主机 control_request、像素偏移及结果序号 | 请求已送达、执行或电机响应时间已测量；其结果序号不能直接连接 DNN 会话 |
| 文件 | 唯一文件名、追加事件、程序/模型 SHA256、退出完整性与丢弃计数 | 已自动保存固件版本、Git 工作区差异、标定和实物安装信息 |
| 相机内参 | 对已有棋盘格图片生成候选参数、独立图片子集的姿态拟合重投影误差 | 自动通过标定验收，或外部角度精度已验证 |

OpenCV RTSP 路径不提供源采样时间。Qt 的 startTime 是显示时间（presentation time），不是曝光时间；未知时为 -1。日志将其单独记为 media_pts_us，capture_mono_us 与 clock_mapping_id 保持 null。尚无时间同步与时钟频偏映射。本地 Qt 6.10.1 QVideoFrame 文档已核对该语义。

## 2. 构建和启用日志

本轮已生成并部署运行库的独立候选：D:/QT/project/untitled7/tmp/research-stage1-app/untitled7.exe（未启动实机）。SHA256：71C55FB364E77199F3B3EEBA165D6913FE0ECDFF2EC0A143074226891EF8DD4C。原正式 EXE 哈希在本轮前后保持不变。该候选仍保留原程序自动发现与控制行为，不是安全隔离采集器；不要在未确认现场条件时直接运行。

先在独立目录构建，不覆盖已验收程序。使用 Qt 6.10.1/MSVC 2022，按根目录 .pro 配置 OpenCV 与 ONNX Runtime。在初始化 vcvars64.bat 的终端中执行：

    qmake D:/QT/project/untitled7/untitled7.pro CONFIG+=release CONFIG-=debug_and_release
    jom -j4

运行库需部署到该独立目录。先退出其他摄像头客户端；是否连接/运动应另行现场确认。下面的环境变量只启用日志，不是无运动模式，正常程序的原有控制仍可工作：

    New-Item -ItemType Directory -Force D:/QT/project/untitled7/tmp/research-capture
    $env:QT_RESEARCH_LOG_DIR = 'D:/QT/project/untitled7/tmp/research-capture'

随后由操作者启动已确认版本的研究程序。关闭程序后删除该进程环境变量可恢复默认无日志模式：

    Remove-Item Env:QT_RESEARCH_LOG_DIR

日志默认关闭；目录必须已存在。异步写入不保存图像、密钥、网络地址和原始控制报文；队列满时丢弃并计数。缺失 run_end、非零 dropped_events 或序号缺口均使离线完整性检查失败。写盘负载仍需现场测量；“异步”不等于没有任何开销。程序异常退出、磁盘不足时保留失败文件，不删除后重跑冒充连续成功。

每次采集另存试验清单：试验 ID、Git SHA 与未提交差异、可执行文件和模型哈希、固件哈希、设备型号/曝光/分辨率/裁剪、方向模式、标定文件哈希、目标身份、距离、场景和操作者。不要记录热点密码或私密图像到 Git。源日志只读保留；工具对现有输出拒绝覆盖。

## 3. 先运行不接实机的检查

    python -m unittest discover -s tests -p test_research_tools.py -v

Qt 检查项目为 tests/researchtrace_test.pro、tests/networkcontroller_test.pro、tests/tracking_concurrency_test.pro，分别 qmake/jom 构建。真实设备与公开模型对照为显式启用的测试，不应为了让通过数更大自动连接硬件。

    python tools/analyze_research_trace.py tmp/research-capture/research-实际文件名.jsonl --output tmp/research-capture/audit-01.json

exit 0 只表示记录结构完整，不代表实验达标。检查 fresh_detection_count、model_loaded、telemetry 等是否覆盖目标试验；空记录与无检测不应写入论文性能表。synchronized_los_ready 在本版始终为 false，optical_end_to_end_latency_ms 始终为 null。

## 4. 相机标定（需要真实棋盘格图片）

使用与跟踪相同的分辨率、裁剪、焦距和方向。固定焦距/对焦条件，覆盖中心、四角、不同倾斜和距离，准备约 20–30 张清晰照片；工具最低要求 12 张有效、非重复图片。图片不能带跟踪框/时间水印。当前工具不打开相机，不自动拍照，不改变旋转；输入图片应已经处于所声明的跟踪方向。

以下 9×6 内角点、20 mm 方格仅是命令格式示例，必须按实际棋盘格测量修改；不能直接拿这些数字作为本机标定数据：

    python tools/calibrate_research_camera.py tmp/calibration-images --columns 9 --rows 6 --square-mm 20 --rotation 3 --output tmp/calibration-candidate.json

候选输出保留图片名称/哈希、OpenCV 版本、训练与留出图片索引、内参和畸变参数。每 5 张留出 1 张；该固定划分不代表场景独立验证。留出图只拟合外参姿态，不重新拟合内参，但重投影误差仍不是外部角度精度。合成投影单元测试也不是实物标定。

候选 reviewed_for_diagnostics=false，horizontal_sign 与 encoder_zero_deg 均为空。人工检查棋盘格覆盖、残差与图像几何，按已确认的物理方向核验符号和编码器零点后，复制成新的审核版本，记录审核依据，才可设置 reviewed_for_diagnostics=true。工具不替你猜测正方向、不修改云台校准。

## 5. 静止目标的近似诊断

先固定基座和俯仰，使用静止且可持续辨认的目标。确保现有急停、限位、故障与遥测看门狗有效；未确认安全条件时不通电运动。之后才由操作者授权低速、小行程水平运动。记录方向、零点、距离和偏心尺寸，不默认无限远。

对采集完成的日志运行近似检查时，明确提供静止目标声明和允许的主机遥测年龄上限，例如格式如下；100 ms 仅是演示参数，需根据实际采集条件选择：

    python tools/analyze_research_trace.py tmp/research-capture/research-实际文件名.jsonl --calibration tmp/calibration-reviewed.json --static-target --max-telemetry-age-ms 100 --output tmp/research-capture/los-diagnostic-01.json

算法只取视频主机接收时刻之前已到达的遥测，不用后来的遥测插值；跨重连、故障、无效校准、过期角度、方向/尺寸不一致均拒绝。检测中心先去畸变再转换水平角；按流、跟踪会话和连接代号分别统计去均值 RMS、95%偏差和峰峰值。没有独立真值，不报告绝对角度误差。检测框中心不是稳定物理特征点，必须人工复核目标与框偏移。

这是主机到达时间的近似诊断，不是论文要求的同步 LOS 验收。本版 acceptance_pass 始终为 false。要进入定量实验，还需增加相机/编码器源采样时间、统一时钟映射与不确定度，或独立测量每条链路延迟并报告近似误差；不能通过修改布尔值绕过该门槛。

## 6. 下一阶段验收

1. 真实图片完成内参/畸变标定并审核。
2. 一次不遮挡的静止目标试验有完整原始日志、来源清单、人工目标身份复核。
3. 建立可解释的时间对齐和安装误差预算，再定义并冻结验收阈值。
4. 通过后才接入离线 Kalman/缺测回放；最后才考虑受限闭环。不得从离线结果推断真实遮挡恢复成功率。

本阶段无需烧录或改板端服务；GitHub 版本发布按用户单独授权执行。暂不覆盖原论文和用户已有研究计划。
