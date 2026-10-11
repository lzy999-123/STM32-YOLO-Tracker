# Luckfox 无线视频部署记录

验证设备：Luckfox Pico Plus，Linux 5.10.160，RTL8188EU/8188ETV USB网卡（0bda:0179）。
这是当前已运行设备的部署脚本和驱动修复记录，不能仅凭相同内核版本认为其他固件的模块ABI也相同。

## 文件与板子路径

| 仓库文件 | 板子路径 |
| --- | --- |
| S99zzzwifi | /etc/init.d/S99zzzwifi |
| S99zzzdiscovery | /etc/init.d/S99zzzdiscovery |
| luckfox-discovery.py | /oem/usr/bin/luckfox-discovery.py |
| luckfox-control.py | /oem/usr/bin/luckfox-control.py |
| S99zzzcontrol | /etc/init.d/S99zzzcontrol |
| S99zzzvideo | /etc/init.d/S99zzzvideo |
| build/install_video_service.sh | 部署时上传到 /tmp，安装后可删除 |
| S99zzzvideoqos | /etc/init.d/S99zzzvideoqos |
| video-qos.c 的构建结果 | /oem/usr/bin/luckfox-video-qos |

两个启动脚本需要可执行权限。发现服务使用板子现有的Python 3和UDP 39093；Qt收到回应后连接回应源地址的RTSP `/live/0`。
发现服务可选读取 `/userdata/cfg/luckfox-control.json` 的 `auth_key`、`bind_address`、`device_id`；文件不存在时不签名并照常运行。
小内存设备可在同一配置中设置 `"embedded_discovery": true`，让控制服务在原有 `select` 主循环同时处理 UDP 39093。每轮最多回应一个发现请求，UART、控制心跳和遥测继续处理；发现回应的格式、设备 ID 和 HMAC 签名复用原实现。配置项缺省为 `false`，保留旧部署方式。

启用时先停止独立发现进程，再重启控制服务，最后执行发现启动脚本：

```sh
/etc/init.d/S99zzzdiscovery stop
/etc/init.d/S99zzzcontrol restart
/etc/init.d/S99zzzdiscovery start
```

只有 UDP 39093 实际绑定成功后，控制进程才写入 `/var/run/luckfox-discovery.integrated.pid`；退出时移除自己持有的标记。发现启动脚本核对该 PID 的命令行，识别正在运行的共用服务并避免再启动一个 Python 解释器。控制子进程已经启动但注册尚未完成时，最多等待五秒；备用守护循环也检查共用服务，防止开机时抢先另起解释器并持续报端口占用。控制服务不可用时，发现脚本仍能启动原来的独立服务；`discovery stop` 仅停止独立服务，共用端口随控制服务关闭。

Wi-Fi脚本读取板子已有的 `/userdata/cfg/wpa_supplicant.conf`；仓库不保存热点密码。
有线管理地址为192.168.8.93/24，无线地址由热点DHCP分配。开机约一分钟完成无线初始化。

## 上电首选手机热点与备用实验室 Wi-Fi

在板端 `/userdata/cfg/wpa_supplicant.conf` 中保留两个 `network` 配置块。2026-10-11 按用户要求调换优先级：手机热点 `Redmi K70E` 设置 `priority=20`，实验室 Wi-Fi `DH4504` 设置 `priority=10`。数值越大越优先；开机扫描不到首选手机热点时，使用可用的实验室 Wi-Fi。两个网络的真实凭据只保存在板端受保护配置中，文件权限为 `600`，不放入仓库。

`S99zzzwifi` 使用后台外壳持续监测连接，保留开机 35 秒等待和首次关联后的 15 秒摄像头初始化等待。网络 ID 或 BSSID 变化时，停止旧的 `wlan0` DHCP 进程、清理该接口的 IPv4 地址和路由，并获取新租约；DHCP 进程退出后重新启动。没有网络时继续等待，避免一次 40 秒等待失败后不再恢复。

服务支持 `start/stop/restart/status`，`start` 会核对 PID 和进程参数，重复执行不会另起监测进程。日志在 `/tmp/luckfox-wifi-start.log`、`/tmp/luckfox-wpa.log` 和 `/tmp/luckfox-wifi-dhcp.log`；`test` 是前台连接检查入口。优先级用于网络选择，当前已连接备用网络时，不承诺首选网络一出现就立即切换。

维护通道使用 SSH 密钥认证并固定核对主机公钥。本次部署关闭了无认证的网络 ADB；板端 `/userdata/cfg/disable-adbd` 标记由已修改的 `S50usbdevice` 在启动时检查。维护私钥保存在电脑受限访问、Git 忽略的本地目录中。无线地址会随 DHCP 改变，操作前重新发现设备并核对身份。

原实验室优先配置的重启验证见 [双 Wi-Fi 优先级学习报告](../../docs/learning/2026-10-11-02-dual-wifi-priority.md)；当前热点优先配置的持久文件与运行参数核对见 [优先级调换报告](../../docs/learning/2026-10-11-07-hotspot-first-wifi-priority.md)。本次调换未重启整板，也未重新测试热点不可见时的备用连接。

## 无线视频的传输与优先级

2026-10-11 按用户要求回退到 `ecd73bf` 视频方案：Qt 使用 OpenCV FFmpeg 后端，TCP 优先，建联失败才回退 UDP；打开超时 5 秒、读取超时 2 秒、无帧巡检门槛 5 秒。保留原生 1280×720、30 FPS 和最新帧替换。当前构建与历史实验边界见 [视频部署说明](../../docs/deploy/VIDEO_720P30.md)。

`video-qos.c` 是小型本地程序，以 root 运行，通过 Linux 5.10 的 `pidfd_open/pidfd_getfd` 给 `/oem/usr/bin/rkipc` 的 IPv4 RTSP TCP 554 及动态 RTP/RTCP UDP 套接字设置 AF41（`IP_TOS=0x88`）。本设备使用的 RTL8188EU 驱动按 `IP_TOS >> 5` 选择 WMM 优先级，因此视频进入优先级 4 的视频队列。控制与发现服务属于其他进程，不受此设置影响。

程序每秒检查一次新套接字，复制的文件描述符当次即关闭，覆盖视频服务重开和新客户端连接。它不创建媒体缓冲或额外视频客户端。初版实测常驻 RSS 84 kB；需在实际目标内核验证 pidfd 调用权限，不能只依据版本号认为其他固件也适用。

使用匹配的 ARM/uclibc SDK 工具链构建，将结果保存在被忽略的构建目录：

```sh
CC=/work/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc \
  sh board/luckfox/build/build_video_qos.sh board/luckfox/video-qos.c /work/luckfox-video-qos
```

**视频 AF41 默认关闭。** 2026-10-11 在实验室 DH4504 网络中，相同旧版 UDP 收流代码的两轮对照表明，强制 AF41 的吞吐与空档明显差于默认套接字优先级；具体数据和边界见本次学习报告。不能将优先级设置成功视为视频更稳定。

部署二进制与启动脚本后赋予执行权限。没有 `/userdata/cfg/enable-video-qos` 标记时，`start` 停止旧检查程序、恢复现有套接字的默认优先级并报告 `disabled`；下次启动同样保持默认优先级。仅在目标链路对照验证有收益后，才创建该标记并运行 `/etc/init.d/S99zzzvideoqos start`。通过 `status` 与 `/tmp/luckfox-video-qos.log` 核对实际状态。

回退优先级并保持下次启动关闭：

```sh
/bin/rm -f /userdata/cfg/enable-video-qos
/etc/init.d/S99zzzvideoqos stop
```

`stop` 会停止检查程序并恢复现有套接字，而非仅停止进程后遗留 AF41。`--once` 只应用一次，`--once --pid PID` 仅检查指定且路径匹配的 rkipc 进程。停止后仍需删除启用标记才能保持下次开机关闭。

视频和云台控制同时运行时，少量控制心跳、指令与遥测使用更高优先级：板端控制服务给自己的 UDP 5005 套接字设置 `IP_TOS=0xe0`（CS7，本设备驱动映射到 WMM 优先级 7）。Windows 上位机通过系统 qWave 的 `QOSTrafficTypeControl` 标记该控制套接字到设备的发送流，关闭连接或对象时释放句柄；设置失败时继续原来的 UDP 通信并记录诊断日志。此设置不增加命令重发，不延长失联门槛。

## 视频服务退出后的恢复

发现与控制在线不代表视频服务在线。连接失败时，先检查 `pidof rkipc`、`netstat -lnt` 的 554 监听与 `/tmp/rkipc-video-service.log`。2026-10-11 本设备的直接故障是 rkipc 收到 SIGTERM 后退出，原厂启动入口没有自动拉起；信号发送来源尚未确认。

上传 `S99zzzvideo` 和 `build/install_video_service.sh`，保留 LF 换行，然后在板端运行：

```sh
sh /tmp/install_video_service.sh /tmp/S99zzzvideo.upload
/etc/init.d/S99zzzvideo status
```

安装器检查原厂脚本的实际结构和 shell 语法，将原文件保存在 `/oem/usr/share/video-service-original/`，只改 `/oem/usr/bin/RkLunch.sh` 的视频启动段与 `RkLunch-stop.sh` 的视频停止段。其他固件结构不匹配时退出，不猜测替换位置。安装期间不重新执行网络、模块加载或整板启动流程。

原厂初始化完成后和后续 init 阶段都调用同一服务。两层 `flock` 分别串行化生命周期操作和保持守护单实例，子进程关闭锁描述符。启动时接管已有 rkipc，避免两个编码器竞争摄像头；检测到多个现有实例时拒绝增加第三个。编码器退出后按 2/4/8/16/30 秒退避重启，稳定运行 15 秒后将等待重置为 2 秒。进程退出清理及重新初始化另需时间，实测一次 SIGTERM 到恢复监听约 10 秒。

服务显式沿用 `/userdata/rkipc.ini` 和既有 IQ，不修改编码规格、Wi-Fi 或 QoS。停止时校验进程的可执行文件和启动时间，防止 PID 复用误杀；正常停止守护与视频进程后不再自动拉起。原厂 stop 入口先停守护再执行原来的清理，避免停止循环中再次启动视频。

每 10 个存活巡检周期检查日志：编码器日志超过 128 KiB 时保留最后 32 KiB，守护日志超过 32 KiB 时保留最后 8 KiB。这是定期截断门槛，写入突发期间可能短暂超过门槛。日志留在 `/tmp`，不写小容量 `/userdata`。

使用 `start/stop/restart/status` 管理视频。配置文件、二进制、IQ 或摄像头节点尚未就绪时继续等待。此守护检测进程退出，画面停止但进程仍存活的故障需要进一步检查传感器和传输。部署与实机检查见 [连接失败修复报告](../../docs/learning/2026-10-11-06-rtsp-video-service-recovery.md)。

实现依据：[Qt 套接字选项说明](https://doc.qt.io/qt-6/qabstractsocket.html#SocketOption-enum) 指明 Windows 不支持该 Qt `IP_TOS` 选项；[Microsoft qWave 流优先级说明](https://learn.microsoft.com/en-us/windows/win32/api/qos2/ne-qos2-qos_traffic_type) 与 [发送流配置说明](https://learn.microsoft.com/en-us/windows/win32/api/qos2/nf-qos2-qosaddsockettoflow) 支持按控制套接字和目的地址配置优先级。API 成功或板端实际 TOS 不等于热点已承诺固定延迟，需同时验证持续视频和控制遥测。

本机修复与尚未完成的验收记录见 [2026-10-11 学习报告](../../docs/learning/2026-10-11-01-video-stutter-transport-recovery.md)。无线到帧间隔、解码帧率与摄像头到屏幕的同步延迟是不同的测量量。

## 驱动依赖与构建记录

启动脚本依赖板子自带的libaes、aes_generic、ctr、ccm、cfg80211、libarc4模块，以及 `/userdata/wifi-driver/` 下的r8188eu、lib80211、lib80211_crypt_wep、lib80211_crypt_ccmp。
编译模块和固件保留在本地诊断目录及已部署板子上，Git仓库记录源代码修正和部署流程。

内核基线：[Luckfox SDK固定提交](https://github.com/LuckfoxTECH/luckfox-pico/tree/824b817f889c2cbff1d48fcdb18ab494a68f69d1/sysdrv/source/kernel)，Linux 5.10.160，GCC 8.3 ARM/uclibc工具链。

`build/` 保留诊断构建环境的脚本，原环境内核目录为 `/work/kernel`、工具链目录为 `/work/toolchain`。
这些脚本需要已准备好的SDK内核树和构建配置，不是全新机器的一键安装程序。
`repair_wifi_abi.py` 的最后上传步骤引用本地构建环境的 `/work/upload_build.py`，归档脚本不包含该传输助手；需要自行拷贝构建结果。

已验证的修复：

- skb与net_device布局按实际板子内核比对，保留IPv6和SDK的Android KABI区块，关闭NETFILTER/XFRM/SKB_EXTENSIONS。
- `prepare_ccmp.py` 把CRYPTO_MINALIGN调整为8，并在CCMP模块初始化的进程上下文提前创建AES-CCM，避免发送时首次创建导致原子上下文等待。
- 启动脚本先加载AES/CTR/CCM，再加载CCMP；临时替换的modprobe路径会恢复。
- 摄像头服务会重启DHCP进程，启动脚本在认证后等待15秒，再恢复有线静态地址和无线租约进程。

历史上曾猜测PREEMPT_COUNT不一致是根因，但启用后仍出现故障；不能把此项写成已经证明的故障原因。
实际已运行的主驱动没有在crypto.h对齐修正后再次重编，修正后的CCMP依赖与该主驱动组合已通过无线视频及重启验证。

早期证据摘要见 [驱动交接记录](../../docs/archive/WIFI_DRIVER_HANDOFF.md) 和 [2026-10-08 阶段记录](../../docs/archive/MILESTONE_2026-10-08.md)。

## 无线云台转发

新增 UDP 5005 控制服务，将电脑发来的坐标/指令转换成 STM32 UART 协议，并回传遥测和实际 ACK。

当前 Pico Plus 接线使用 UART2_M1：GPIO1_B2/TX → STM32 PA3/RX，GPIO1_B3/RX ← PA2/TX，共地。
实机已释放 FIQ 控制台并启用 `/dev/ttyS2`；配置样例见 `luckfox-control.example.json`，参考 DT 片段见 `uart2-stm32-overlay.dts`。
服务支持 `start/stop/restart/status`，日志位于 `/tmp/luckfox-control.log`。
只使用标准库并以 `python3 -S` 启动，避免小内存板子承担不必要的模块开销。
目前已确认 STM32 遥测、指令 ACK 和模式切换；用户尚未接入云台，运动闭环仍待验收。
部署与当前 UART 启用限制见根目录 [无线控制说明](../../WIRELESS_CONTROL.md)。
`/userdata/cfg/luckfox-control.json` 必须使用确认后的实际 UART；配置缺失时不启动服务。
网络命令按请求编号去重，UART 非幂等命令不盲目重发；UART ACK 超时只报告失败、保留会话。
新版脚本还读取实际 `wlan0` 关联状态，在新 STM32 固件能力确认后每 2 秒发送 Wi-Fi 名称；不依赖 Qt 会话，不刷新控制心跳。PC13 短按切页、长按切模式和长名称滚动的配套固件、协议与验收说明见 [STM32 OLED Wi-Fi 页面](../stm32/README.md)。2026-10-11 已完成实板脚本部署及 UART 能力 / 名称上报检查，用户确认短按 PC13 后名称正确；长名称及长按的现场验收尚未执行。
断网或 bye 时只发送停止跟踪（0x12）并停止 UART 心跳，不主动回中，由 STM32 链路超时在自动模式回中。
两个服务由启动脚本的 `supervise` 外壳守护，异常退出后自动重启；UART 读写错误会关闭并退避重开。
`auth_key` 配置后控制和发现消息使用 HMAC-SHA256 签名，样例配置中为空（不认证）；配置与密钥分发见
[无线控制说明](../../WIRELESS_CONTROL.md) 的“消息认证配置”，真实密钥不得提交。
