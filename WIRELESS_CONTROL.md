# 无线云台控制与新版 UI

## 系统路径

电脑 Qt ← RTSP 视频 ← Luckfox。
电脑 Qt → UDP 5005 目标偏移/指令 → Luckfox → UART → STM32 → 云台。
STM32 → UART 角度/模式/ACK → Luckfox → UDP 5005 → 电脑 Qt。

电脑不使用 COM、不枚举串口、不依赖 Qt SerialPort。识别与跟踪继续在电脑执行。

## 界面使用

- 默认布局采用用户保存的 1100×700 窗口，视频初始区域为 832×468；调整窗口时保持画面比例。
- 顶部 Wi-Fi 名称由 Windows 原生 WLAN API 在后台读取，每 5s 刷新；权限不足时显示无法读取，不伪造名称。
- 地址显示实际发现或手动输入的 RTSP 设备主机地址。
- 程序启动后自动发现 Luckfox 并连接控制服务，未发现或断线时自动重试；启动时不开视频。
- 点击“打开摄像头”后才连接视频；关闭视频或视频失败时保留控制连接，同时停止跟踪控制。
- 顶部连接状态和指示灯表示视频通道；底部表示控制服务和 STM32 状态。
- “刷新”重新读取 Wi-Fi 并查找设备，按钮与状态栏显示进度和结果。视频关闭时不会自动打开；地址未变且视频已打开时保持原视频连接。
- 开始/停止、模式切换、手动方向、回中均通过网络发送；模式和角度以 STM32 回传为准。
- 未获得有效 STM32 遥测时控制按钮禁用；断线后清除旧目标，重连不会自动恢复运动。
- 固定手动步长为 0.5°，与当前 STM32 协议一致；画面旋转入口位于底部。

## Luckfox 部署

仓库 `board/luckfox/luckfox-control.py` 对应 `/oem/usr/bin/luckfox-control.py`，
`board/luckfox/S99zzzcontrol` 对应 `/etc/init.d/S99zzzcontrol`，文件需可执行、LF 换行。
程序仅使用板子现有 Python 标准库，不要求 pyserial。

板端配置文件 `/userdata/cfg/luckfox-control.json` 必须明确填写实际连接 STM32 的 UART。
本项目当前接线已经确认使用 `/dev/ttyS2`，配置示例为 `board/luckfox/luckfox-control.example.json`。
其他板子仍须根据接线、引脚复用和设备节点确认，不能直接照抄。
串口使用 115200、8 位数据位、1 停止位、无校验、无流控。
程序拒绝占用 `/proc/cmdline` 中的系统控制台 UART。

确认 UART 与接线后，用 `/etc/init.d/S99zzzcontrol start` 启动；开机脚本也使用同一配置。
配置不存在时，启动脚本明确报告未配置并不启动服务。
Qt 可以先使用视频，控制服务不可用时会继续握手并显示云台离线。

可选配置项：

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `bind_address` | `0.0.0.0` | 控制（UDP 5005）与发现（UDP 39093）监听地址。绑定具体地址后发现服务收不到广播，Qt 会退回本网段单播查找 |
| `auth_key` | 空 | HMAC-SHA256 密钥，十六进制、至少 32 个字符；空表示不认证（启动日志会警告） |
| `device_id` | 网卡 MAC | 仅发现服务使用；默认取 eth0 MAC，eth0 不存在时退回 wlan0 或其他网卡 |

### 守护与自恢复

`S99zzzcontrol`、`S99zzzdiscovery` 启动的是 `脚本 supervise` 守护外壳，PID 文件记录外壳 PID，
子进程 PID 记录在 `*.child.pid`；状态检查会核对 `/proc/<pid>/cmdline`，避免 PID 复用误判。
Python 进程退出后外壳 2s 重启；配置无效时 Python 以退出码 2 结束，外壳不再重启并保留日志。
`stop` 先结束外壳（外壳转发 TERM 给子进程），再确认子进程退出。
Python 内部：UDP 收发的 OSError 只记录日志（同类错误 30s 内只记一次，避免写满内存盘 /tmp）；
UART 读写出错时关闭串口，按 0.5s 起、最长 5s 的退避重开，期间等待中的命令按 ACK 超时报告失败；
其他未预料异常由进程内监督循环记录堆栈，按 1s 起、最长 30s 退避后重建服务。

### 控制策略（2026-10 调整）

- UART ACK 450ms 超时：只向电脑报告 `command_failed`，保留会话，不停止跟踪、不回中。
- 会话超时或 bye：只发送 CMD 0x12 停止跟踪并停止 UART 心跳，不发送 0x02 回中；
  STM32 1s 链路超时后仅在自动模式回中，手动模式保持角度。
- 目标坐标每个新序号只转发一次（最短间隔 20ms，间隔内只发最新值），不重复发送同一坐标。
- 状态消息增加 `flags`（故障/急停/链路丢失），详见 `PROTOCOL.md`。

### 消息认证配置

配置密钥后，控制与发现消息使用 HMAC-SHA256 签名，详见 `PROTOCOL.md`“认证封装”。步骤：

1. 在电脑上生成密钥：`python -c "import secrets;print(secrets.token_hex(32))"`。
2. 板端：编辑 `/userdata/cfg/luckfox-control.json`，把密钥填入 `auth_key`，然后
   `/etc/init.d/S99zzzcontrol restart`、`/etc/init.d/S99zzzdiscovery stop && /etc/init.d/S99zzzdiscovery start`。
   注意文件权限（建议 `chmod 600`），不要把含真实密钥的配置提交到仓库或贴进报告。
3. 电脑端：把同一密钥写入 `untitled7.exe` 所在目录的 `luckfox-control.key`（单行十六进制），
   或设置环境变量 `LUCKFOX_CONTROL_KEY`（优先于文件）。该文件已加入 `.gitignore`。
4. 两端必须同时启用或同时停用。只有一端有密钥时，控制会话无法建立、发现也会失败；
   电脑端密钥格式错误时直接报告错误，不会退回明文。

限制：签名只保证来源和完整性，不加密；板端重启后旧会话记录清空，重启前截获的报文理论上可被重放；
发现请求本身未签名，任何人都能得到设备的 device_id。

### 当前接线与 UART2 配置

| Luckfox Pico Plus 引脚 | UART2_M1 功能 | STM32 接口 |
| --- | --- | --- |
| 引脚 1 / GPIO1_B2 | TX | PA3 / USART2 RX |
| 引脚 2 / GPIO1_B3 | RX | PA2 / USART2 TX |
| GND | 共地 | GND |

当前接线方向正确。板子原先将这对引脚用于 FIQ 调试控制台，普通 UART2 节点禁用。
实机已将 `/fiq-debugger` 设为 disabled、`/serial@ff4c0000` 设为 okay，保留 UART2_M1 pinctrl；
启动参数移除 UART2 earlycon、将 `console=ttyFIQ0` 改为 `console=tty0`，其余存储启动配置保留。
参考设备树片段见 `board/luckfox/uart2-stm32-overlay.dts`。运行时 overlay 不能代替重启释放 FIQ。

本次修改前已备份完整 boot 分区、env、inittab 和 luckfox.cfg；只更新 FIT 头和 DTB，
重新计算 DTB 大小与 SHA256，确认内核及资源字节不变，再读取整个 boot 分区核对。
本机备份仅保留在忽略的 `tmp/uart2-backup-20261008/`；板端保留
`/root/.luckfox-uart2-backup/boot.prefix.before.img`。实际恢复必须使用本机对应备份，不能使用别的板子的启动文件。

重启后 `/dev/ttyS2` 指向 `ff4c0000.serial`，不再作为 Linux 控制台。
控制服务使用 Python 标准库、`python3 -S`，减少板端内存占用；启动失败返回非零并保留错误日志。
检查服务用 `/etc/init.d/S99zzzcontrol status`，日志在 `/tmp/luckfox-control.log`。
板子资源较少，维护时避免额外运行占用大量内存的 Python 检查进程。

## 2026-10-08 实际验证范围

已确认：

- Qt Release 编译、链接和真实启动通过；构建已移除 SerialPort。
- 新界面显示真实电脑 Wi-Fi 名称、发现地址及 1280×720 无线画面，云台未连通时正确禁用控制。
- 10 项 Python 桥接测试通过；2 项 Qt 网络控制测试通过。
- 真实 Wi-Fi 下，Qt 网络控制类与 Luckfox 上运行的桥接代码完成握手、手动步进、模式切换、负偏移转发、状态回传和回中。
  此项使用隔离 UDP 5006 与软件模拟 STM32，不打开任何硬件 UART。
  故意丢弃一次网络 ACK 后，手动左转的模拟 UART 执行次数仍为 1。
- 板端脚本已上传，板上 Python 编译及标准库检查通过。
- UART2 已按用户接线启用；5 秒被动接收得到 520 字节、52 帧 CRC 正确的实体 STM32 遥测。
- 生产 UDP 5005 服务已部署并启动。真实 STM32 对方向、回中、模式、开始/停止指令返回 ACK，模式按指令切换 0↔1。
- 真实 Qt 界面随实体遥测切换手动/自动，自动模式禁用手动方向按钮；视频与控制同时运行。

尚未确认：

- 实体 STM32 的模式、角度、ACK 与云台运动闭环。
- 同步摄像头到屏幕的端到端延迟。

用户已确认当前下位机没有连接云台。实机遥测的两轴角度一直为 0；ACK 只证明下位机接收并确认命令，
不能据此证明舵机转动、0.5° 步进、回中运动或断网运动保护已经通过。
目标偏移已经过网络/协议验证，实体运动和角度闭环须等连接云台后单独验收。

联调曾因额外进程及控制服务占用内存触发 OOM，系统结束 rkipc。已经精简服务依赖、恢复视频，
后续以同时运行及重启检查为准；不能仅凭早先端口监听就宣布视频稳定。

本地日志、测试结果和私人摄像头截图仅保留在被忽略的 `tmp/` 中。
