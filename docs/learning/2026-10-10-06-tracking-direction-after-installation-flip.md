# 学习报告：安装方向校正后的跟踪反向修复

- 日期：2026-10-10（Asia/Shanghai）
- 本次提交主题：纠正垂直翻转安装下自动跟踪的Y方向（阶段未提交）
- 修改前基线：`fae9e3166f83c7bb69791ac882ce58a998de21b3`加已有未提交工作；本轮改前上位机EXE SHA256 `B16CA65743A48FC7D88EC4F903913434F28C9E0FFE7B38CEE78511601078A6AD`
- 涉及模块：mainwindow.cpp、Qt跟踪/网络回归、Luckfox桥接回归、协议/操作说明、学习报告索引；只读取STM32源码并在主机上验证其现有控制函数

## 1. 本次目标与完成情况

用户观察到跟踪手机时，手机向下移动，摄像头却向上转；本轮用户确认现有手动“向下”按钮确实使摄像头向下。已核对画面变换、YOLO/CSRT偏移计算、Qt网络包、Luckfox UART封装与STM32自动/手动控制的符号关系。

确认错误位于`MainWindow::controlOffsets()`：跟踪使用垂直翻转后正常画面中的目标位置，但发送前又将Y取反。当前安装中MirrorV是安装方向校正，应该直接发送校正画面的偏移。删除这次Y取反；水平镜像原X补偿及旋转模式原行为保留。

修正版已编译并替换原Release目录的上位机程序，源与目标EXE哈希相同。未修改、烧录STM32固件，未部署Luckfox脚本，也未开启实体自动跟踪。软件链路回归通过；新程序在实物上的自动跟随仍需用户重新打开后确认。

## 2. 本次学到的知识

- 坐标方向必须按实际安装定义 → 用户的正常画面需要MirrorV → 不能把所有镜像操作都归为纯显示偏好。
- 计算坐标与控制符号要逐段检查 → 目标中心减画面中心，向下Y为正；STM32负增益将正Y变为负垂直速度；手动向下也减小垂直角度 → 通信层必须保留这个符号。
- 回归要关联实际场景 → 旧测试只断言MirrorV必定取反，因此保护了错误假设 → 新测试输入下方/上方目标框并接收真实Qt UDP包，再送入桥接和真实C控制函数。
- 软件控制方向与实物验收分开 → 主机C检查能确认目标角度增减、不能替代电机实际转动 → 报告只记录已执行的层级。

## 3. 遇到的问题与解决过程

| 现象 | 证据与原因 | 处理方法 | 结果 |
| --- | --- | --- | --- |
| 手机向下、自动跟踪向上 | 用户观察；旧Qt测试输入下方80像素，实际UDP输出Y=-80；经过桥接与真实C函数，垂直目标200°变为200.12°，与手动向上同符号 | 删除MirrorV发送前的Y取反，保持校正后的偏移 | 修正版Y=+80，真实C垂直目标变为199.88°，与手动向下一致 |
| 先前默认翻转修复保留了错误控制假设 | [报告04](2026-10-10-04-default-vertical-camera-flip.md)沿用原MirrorV取反并明确未验证实物方向；旧测试验证的是该假设 | 保留历史报告，在本报告纠正并更新当前协议/说明和索引 | 以本次场景回归取代旧MirrorV取反期望 |
| CSRT新测试首次受模拟遥测超时及连接重置干扰 | 首次方向复现中CSRT-down失去目标；调整准备顺序后，连接初始化又按正常保护逻辑清除了预建跟踪器，导致5个CSRT场景没有包 | 用独立TrackingEngine先准备真实CSRT，再连接模拟服务，将当前会话的真实目标信号接入MainWindow发送路径 | 最终10个方向场景及相关Qt回归均通过；两份失败日志保留，未把失败写为通过 |
| 主机校验脚本读取Qt日志时报解码错误 | Qt文本日志含Windows本地编码的诊断文字，直接UTF-8读取失败；方向JSON本身是ASCII | 解析时容忍无关诊断文字的编码，保留实际方向JSON | 真实C校验最终退出0 |
| 原上位机仍运行，无法安全替换 | 检查发现原Release EXE进程；用户确认已停止跟踪并关闭 | 再次检查进程不存在后复制修正版，核对SHA256 | 原目录更新完成 |

## 4. 验证与证据

- Qt应用Release构建成功：`build/manual-step-Release/tracking-direction-build.log`。
- 最终Qt测试构建成功：`build/manual-step-tests/tracking-direction-build-final.log`。
- 修改前场景复现日志：`build/manual-step-tests/tracking-direction-before-results.txt`。YOLO-down发送Y=-80、YOLO-up发送Y=+80；首次共8 passed、4 failed，其中3个是垂直方向断言失败，1个是CSRT-down受模拟连接超时清除目标。
- 中间测试准备问题日志：`build/manual-step-tests/tracking-direction-fixture-failure.txt`，14 passed、5 failed；5个失败均为预建CSRT在连接初始化后没有目标包。
- 最终Qt回归：`build/manual-step-tests/tracking-direction-results.txt`，**19 passed、0 failed、0 skipped**（包括初始化/收尾）。涵盖YOLO和真实CSRT各5个方向场景、安装校正/菜单状态、只发新结果、旧会话丢弃、HOLD零偏移、时间图层不污染跟踪帧、手动步长与模式、视频关闭/刷新保持控制连接。
- YOLO测试注入已知检测框，调用真实偏移计算和发包；CSRT使用合成纹理图像运行真实跟踪器。两者都使用回环模拟控制服务，没有私人摄像头截图和实体运动。
- Luckfox桥接回归：`python -m unittest discover -s tests -p test_control_bridge.py`，**29 tests、OK**；新增符号保留检查覆盖上下左右、居中和int16两端。日志：`build/manual-step-tests/tracking-direction-bridge-results.txt`。
- 实际C源码主机检查：`build/manual-step-tests/check_tracking_direction.py`从现有正式STM32源码提取`Proto_ParseByte`、`AutoVelocity`、`AutoStep`、`AutoControl_Update`和逻辑角度函数，硬件I/O使用桩。**原253项坐标/启动/限位/回中/手动检查通过；10个最终Qt UDP包经过实际桥接与UART解析后，自动目标角度方向全部与对应手动命令一致**。旧下方场景在相同链路中复现向上目标增量。日志：`build/manual-step-tests/tracking-direction-firmware-results.txt`。
- 在上述主机C测试中，80像素、10ms控制周期对应约0.12°目标变化；它是软件控制输出，不是实际电机移动量、响应时间或端到端延迟。
- 原源码/测试/说明与EXE备份：被忽略的`build/tracking-direction-before/`。诊断日志和生成的C/DLL均留在被忽略的build目录。
- 更新EXE：`build/Desktop_Qt_6_10_1_MSVC2022_64bit-Release/release/untitled7.exe`；SHA256 `DE1ABF3F58F82F6AC3A2388D3DA0D9E1B318166C62645B602500D1F5A6EC1644`，与本次构建输出一致。
- 现有STM32 HEX SHA256仍为`450A2F4808FAC7FEB55353E2721D84813511E65C2B3B7EC75F14D08A383D5C90`。本轮没有重新编译或烧录固件，没有重校准或修改角度坐标。

## 5. 以后做项目要注意什么

每次调整摄像头安装方向，都检查“正常画面中的目标偏移 → 下发符号 → 逻辑角度增减 → 手动方向”的完整关系。先用真实数据包复现，再改对应层，避免同时改上下位机符号掩盖问题。跟踪会话重置是现有保护逻辑，测试准备不能把旧目标跨连接保留下来。更新可执行文件前确认旧程序已退出，并核对构建与交付文件一致。

## 6. 当前不足与下一步

用户重新打开已更新程序，在默认垂直翻转正常画面下重新选手机，确认上下移动时云台与目标方向一致；可同时核对左右。当前软件符号链路已经验证，修正版实际电机跟随尚未执行，本报告不宣称项目最终验收完成。

## 7. 对最终项目总结的贡献

记录安装校正与控制坐标混用造成的反向跟踪、旧断言如何固化错误假设、跨Qt/Python/真实C函数复现和验证的方法，以及本次对报告04未验证假设的纠正。
