# 优化说明（2026-07-26 串口协议 v2 改造）

> 用途：记录本轮"量产化第一阶段"改了什么、为什么改、改在哪，方便日后查找。
> 相关文档：协议规范 `PROTOCOL.md`；交割/验收清单 `HANDOVER.md`。

## 一句话总结

重写了上下位机之间的串口通信层：从"无校验、无应答、无心跳的两套裸协议"升级为
"统一二进制帧 + CRC8 + ACK 重传 + 心跳 + 断线自愈 + 下位机失控保护"，
消除一个会导致解析错位的协议 bug 和一个串口"失聪"隐患。

## 修复的问题 → 方案 → 代码位置

### 1. 负偏移量与帧头冲突（协议 bug，会实际出错）
- **问题**：v1 数据帧用 0xFF 做帧头，但目标在画面左/上方时 offset 为负，数据字节本身
  就会出现 0xFF（如 offsetX=-1 → FF FF），下位机按字节找帧头会错位解析，云台方向跳变。
- **方案**：新帧格式 `A5 5A | LEN | TYPE | PAYLOAD | CRC8`，靠 LEN+CRC 定界，
  数据区允许任意字节值；解析失败逐字节滑动重同步。
- **位置**：`serialcontroller.cpp` `parseRxBuffer()`；下位机 `main.c` `HAL_UART_RxCpltCallback`。

### 2. 上位机死机/断线后云台失控（安全问题）
- **问题**：v1 下目标丢失、上位机崩溃、USB 拔出三种情况下位机都只是"收不到数据"，
  无法区分；云台保持最后指令，无任何保护。
- **方案**：两层超时分层处理——
  - 200ms 无跟踪数据（目标短暂丢失）→ 偏移清零，云台**保持**当前角度；
  - 1000ms 无任何合法帧（含心跳，即上位机失联）→ 停止跟踪、**自动回中**、PID 复位，
    OLED 显示 `LINK LOST!`，链路恢复后自动解除。
  - 上位机每 200ms 发心跳帧维持链路。
- **位置**：心跳 `serialcontroller.cpp` `sendHeartbeat()`；失控保护 `main.c` 主循环
  `LINK_TIMEOUT_MS` 段；数据超时 `main.c` TIM2 中断（原有逻辑保留）。

### 3. USB 拔出后上位机静默失败
- **问题**：v1 未监听 `QSerialPort::errorOccurred`，拔线后 write 静默丢弃，
  UI 一直显示绿灯"已连接"。
- **方案**：监听 errorOccurred → 关端口、UI 变黄显示"重连中"、每 2s 自动重连，
  重连成功自动恢复；另加遥测 1s 超时的"下位机离线"检测（线通但板子没反应也能发现）。
- **位置**：`serialcontroller.cpp` `handleSerialError()` / `tryReconnect()` /
  `checkDeviceOnline()`；UI 响应在 `mainwindow.cpp` 构造函数四个新信号的 connect。

### 4. 关键命令丢包无感知（如"停止跟踪"丢了云台停不下来）
- **问题**：v1 命令一次性发送，无确认。
- **方案**：命令帧带 ACK：下位机执行后回 0x82 应答；上位机 150ms 未收到则重发，
  最多 3 次，全部失败发 `commandFailed` 信号并在日志告警。程序退出时
  `shutdownGimbal()` 发停止+回中并等待写出；即使全丢，下位机 1s 失控保护兜底回中。
- **位置**：`serialcontroller.cpp` ACK 队列（`sendCommand`/`handleAckTimeout`/
  `onAckReceived`）；下位机 `main.c` `pending_ack_flag` + 主循环 ACK 优先发送。

### 5. 串口溢出后下位机永久"失聪"
- **问题**：HAL 库 UART 发生 ORE（溢出）等错误后接收中断停摆，且原工程未处理，
  之后再也收不到任何数据，只能断电重启。
- **方案**：新增 `HAL_UART_ErrorCallback`：清 ORE/NE/FE/PE 标志并重挂 `HAL_UART_Receive_IT`。
- **位置**：`main.c` `HAL_UART_ErrorCallback`。

### 6. 协议杂项
- 双向统一为二进制帧（v1 是发送二进制 + 接收 ASCII 两套风格）；遥测改 0x81 帧
  （mode + 角度×10 int16 大端），角度显示精度不变。
- 上位机命令魔数（0x01/0x02/0x11/0x12）替换为 `SerialController::Cmd*` 枚举。
- 下位机发送帧前检查 `gState` 防止覆盖在途帧；ACK 优先于遥测。

## 验证状态

- 上位机：MSVC2022 Release 编译链接通过（`build\protocheck\release\untitled7.exe`）。
- 下位机：Keil V5.06 编译通过，0 Error 0 Warning。
- **硬件联调未做**：按 `HANDOVER.md` 第 5 节 7 条清单验收（重点：拔 USB、遮挡目标两条）。

## 回滚

- 下位机原 v1 代码备份：`Core/Src/main.c.v1.bak`。
- 上位机 v1 在 git 历史中（本轮改动提交前的版本）。
- v1/v2 互不兼容，两侧必须同版本。

## 尚未做的优化（量产化差距分析中列出、本轮未动）

1. 配置体系：模型名/阈值/相机翻转/串口参数仍编译期写死；"bottle" 类硬编码偏好未移除。
2. 相机恢复重锁逻辑是死代码（`m_forceResetTracking` 永不为 true）。
3. 特征跟踪重活（CSRT/ORB/模板匹配）仍在 UI 主线程。
4. 日志系统（qDebug 残留、UI 日志无上限）、单元测试、CI、Release 打包安装器。
5. 模型许可证确认（Ultralytics AGPL 商用问题）、ONNX Runtime GPU→CPU/DirectML 部署简化。
6. 仓库清理（一次性 python 脚本、草稿 cpp、git 中的 yolov8n.pt）。
