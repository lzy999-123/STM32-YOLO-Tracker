# 交割文档：串口协议 v2 改造（全部完成，待烧录联调验收）

> 状态：**上位机 + 下位机代码均已改完并编译通过**。剩余工作只有：烧录固件 → 按第 5 节
> 验收清单联调 → git 提交。协议规范见同目录 `PROTOCOL.md`。

## 1. 背景

- 上位机：本目录（`D:\QT\project\untitled7`），Qt 6.10.1 + OpenCV/ONNX YOLO 跟踪，
  串口发目标像素偏移给 STM32 控制两轴舵机云台。
- 下位机：`D:\STM32CubeMx\hal_stm32\科教协同下位机-1`（Keil 工程在 `MDK-ARM\servo_duoji.uvprojx`，当前入口为 `Core\Src\main_gimbal.c`）。
- 旧协议（v1）问题：数据帧头 0xFF 与负偏移数据字节冲突会错位解析；无 CRC/ACK/心跳；
  拔线后上位机静默失败；下位机无法区分"目标丢失/上位机死机/断线"；UART 溢出错误后接收永久停摆。
- v2 方案：双向统一二进制帧 `A5 5A | LEN | TYPE | PAYLOAD | CRC8` + 心跳 + ACK 重传 +
  下位机 1s 链路超时失控保护（自动回中）。

## 2. 改动清单（已全部完成）

### 上位机（已编译验证：qmake + MSVC2022 Release，链接通过）

| 文件 | 改动 |
|------|------|
| `PROTOCOL.md` | 新增。协议 v2 完整规范 |
| `serialcontroller.h` | 重写。新增信号 `commandFailed / connectionLost / reconnected / deviceOnlineChanged`；`telemetryReceived` 改为 `(int mode, float angle1, float angle2)`；新增 `shutdownGimbal()`、`Command` 枚举 |
| `serialcontroller.cpp` | 重写。CRC8(poly 0x07/init 0x00)、逐字节重同步解析、200ms 心跳、命令 ACK 队列（150ms 超时×3 重传）、`errorOccurred` 断线检测 + 2s 自动重连、遥测 1s 超时判下位机在线 |
| `mainwindow.cpp` | 构造函数连接上述 4 个新信号（断线→LED 黄/按钮置灰，重连→恢复，离线→label_11 显示"离线"，命令失败→日志）；telemetry lambda 改新签名并格式化角度显示；`closeEvent` 改用 `shutdownGimbal()`；命令魔数全部替换为 `SerialController::Cmd*` 枚举 |

### 手动模式步进扩展（2026-08-21）

上位机四个方向按钮现通过 `CMD` 单字节命令 `0x20~0x23` 控制下位机手动步进，每次固定
改变 0.5°；按钮仅在遥测确认的手动模式下启用。下位机 `Core/Src/main_gimbal.c` 对这四个命令
在手动模式执行目标角度步进，在自动模式仅回 ACK、不改变目标角度。两侧代码和 `PROTOCOL.md`
必须同步烧录/部署，否则新按钮命令不会生效。

### 下位机（已编译验证：Keil V5.06，0 Error 0 Warning）

当前协议实现位于 `Core/Src/main_gimbal.c`；原 `main.c` 不参与当前目标构建。

| 位置 | 改动 |
|------|------|
| 宏定义区 | 新增 PROTO_* 帧常量、`TRACK_DATA_TIMEOUT_MS=200`、`LINK_TIMEOUT_MS=1000`；删除 `PACKET_LEN` |
| 变量区 | 新增 `last_link_time / link_lost_latched / pending_ack_cmd / pending_ack_flag`；删除废弃的 `uart_rx_buf / uart_rx_idx / uart_tx_buf` |
| `HAL_UART_RxCpltCallback` | 整体替换为 v2 帧解析状态机（找 A5→5A→LEN→TYPE+PAYLOAD→CRC），保留 500ms 字节间超时复位；任意合法帧刷新 `last_link_time` |
| `proto_handle_command` | 命令语义与 v1 逐字一致（0x01/0x02/0x11/0x12），执行后置 ACK 待发标志 |
| `HAL_UART_ErrorCallback` | 新增。清 ORE/NE/FE/PE 标志并重挂接收中断（修复溢出后串口"失聪"） |
| `proto_send_frame` | 组帧 + `HAL_UART_Transmit_IT`，先检查 `gState` 再写静态缓冲防覆盖在途帧 |
| 遥测 | 100ms 周期与按键切模式两处均改为二进制 0x81 帧（mode + 角度×10 大端 int16），ACK 优先于遥测 |
| 主循环失控保护 | 自动模式下 `last_link_time` 超 1s → 边沿锁存：停跟踪 + 回中 + PID 复位，OLED 第 4 行显示 `LINK LOST!`；链路恢复自动解除；手动模式不受影响；上电初始化时间戳防误报 |
| TIM2 中断 | 逻辑未动，仅 200ms 魔数改用常量 |

## 3. 关键一致性参数（联调排错先查这里）

| 项 | 值（两侧必须一致） |
|----|----|
| CRC8 | 多项式 0x07，初值 0x00，覆盖 LEN+TYPE+PAYLOAD |
| 字节序 | 大端（高字节在前） |
| 遥测角度 | int16，实际角度×10 |
| 心跳周期 / 链路超时 | 200ms / 1000ms |
| ACK 超时 / 重发 | 150ms / 3 次 |
| 波特率 | 115200-8-N-1 |

## 4. 待办（人工执行）

1. **烧录固件**：Keil 打开下位机 `MDK-ARM\servo_duoji.uvprojx`（已重新编译），或用
   `D:\STM32CubeMx\hal_stm32\科教协同下位机-1\MDK-ARM\servo_duoji\servo_duoji.hex` 烧录。
2. **上位机运行**：新构建产物在 `build\protocheck\release\untitled7.exe`（注意该目录 PATH 需
   有 OpenCV/ONNXRuntime/Qt 的 DLL；日常仍可用 Qt Creator 按原 kit 构建运行）。
3. 按第 5 节联调验收。
4. 验收通过后 git 提交：`serialcontroller.* mainwindow.cpp PROTOCOL.md HANDOVER.md`；
   下位机目录不在版本控制内，建议将整个工程目录纳入 git 或至少保留 `main.c.v1.bak`。

## 5. 验收清单

1. **正常链路**：打开串口 → 日志出现"下位机已上线"，遥测角度实时刷新；锁定目标后云台跟踪行为与 v1 一致。
2. **命令 ACK**：点"回中/切模式/开始跟踪"，下位机执行且日志无 `commandFailed`。
3. **拔 USB**：上位机 1s 内 LED 变黄、显示"重连中"；下位机 1s 后自动回中、OLED 显示 `LINK LOST!`；
   重新插上后上位机自动重连（LED 恢复绿）、下位机恢复正常（LINK LOST 消失）。
4. **杀上位机进程**（模拟死机）：下位机 1s 后回中 + LINK LOST。
5. **目标丢失**（遮挡目标但串口在连）：云台保持当前角度，OLED 显示 `PC Data Waiting` 而**不是**
   LINK LOST（心跳仍维持链路——这条验证数据超时与链路保护的分层是否正确）。
6. **负偏移**：目标在画面左上（offset 为负）时方向正确无跳变（v1 的 0xFF 冲突已消除）。
7. **退出程序**：关闭上位机窗口，云台停止跟踪并回中。

## 6. 回滚方式

- 下位机：当前工程使用 `Core\Src\main_gimbal.c`；如需恢复旧协议，应使用单独备份工程，不要覆盖当前入口文件。
- 上位机：`git checkout -- serialcontroller.h serialcontroller.cpp mainwindow.cpp`
  （前提是改动前的版本已在 git 中；当前 git 里的正是 v1 版本）。
- 注意 v1/v2 协议互不兼容，两侧必须同版本，只回滚一侧会完全无法通信。

## 7. 已知边界与后续建议（本次未处理）

- 命令在下位机 ISR 上下文执行（与 v1 相同），耗时可接受但非最佳实践；后续可改为主循环消费。
- 上位机 `m_forceResetTracking` 死代码（相机恢复重锁逻辑永不执行）仍在，属于量产化清单中
  "故障自愈"一项，与本次协议改造无关。
- 心跳/超时参数如需调整，上位机在 `serialcontroller.cpp` 匿名命名空间常量区，下位机在
  `main_gimbal.c` 宏定义区，两侧需同步修改。下位机逻辑限位和上电动作见
  `Core/Inc/gimbal_config.h`。
