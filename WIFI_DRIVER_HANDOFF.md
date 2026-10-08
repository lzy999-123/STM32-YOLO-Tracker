# Luckfox 无线驱动与 RTSP 验证记录

## 2026-10-08 最新结果

- USB 网卡 RTL8188EU/8188ETV，USB ID `0bda:0179`；内核 `5.10.160`。
- 已成功加载 r8188eu，关联 Redmi K70E，WPA2-PSK/CCMP 认证完成。
- DHCP 地址 `10.226.35.30/24`，网关 `10.226.35.207`。地址由热点分配，可能改变。
- PC `10.226.35.174` 的路由已确认使用 WLAN。
- 已实际打开 `rtsp://10.226.35.30/live/0` 并解码 1280x720 图像。
- 第一次自动启动验证：软件重启后自动认证与 DHCP 成功，10秒接收429帧，保存 `tmp/wifi-rtsp-result.json` 与 `tmp/luckfox-wifi-live.png`。接收数量受缓冲影响，不作为编码帧率或端到端延迟证据。
- 开机脚本 `/etc/init.d/S99zzzwifi` 已安装。它等待35秒，显式加载AES/CTR/CCM和驱动依赖，提前创建加密器，启动supplicant，认证完成后等待15秒避开camera DHCP重启，再恢复eth0静态地址和无线DHCP租约进程。
- 第二次重启验证完成：自动认证、DHCP租约进程、rkipc均正常；eth0静态192.168.8.93与wlan0地址10.226.35.30同时保留；modprobe已恢复/sbin/modprobe；内核日志无BUG/Oops。重启后再次通过PC WLAN解码1280x720视频。

## 已确认的故障与修复

1. SDK与实际内核的 skb/net_device ABI不同。已按实际内核反汇编比对：skb.len80、data164、mac_header142、pkt_type偏移96；net_device.dev_addr460、sizeof1224、private起始1248。保留IPv6和Android KABI区块，关闭NETFILTER/XFRM/SKB_EXTENSIONS。
2. TX原子上下文首次创建AES-CCM，触发模块请求和等待，产生 `scheduling while atomic`。CCMP模块现在在进程上下文预先创建并释放 `ccm(aes)`，失败则拒绝加载。
3. SDK加密结构对齐64，实际内核8，造成释放加密器时Oops。`crypto.h`的CRYPTO_MINALIGN改为8，CCMP依赖已重编。
4. 摄像头服务在无线连接后会重启DHCP客户端，并启动eth0 DHCP。新一次日志无Oops，eth0地址被清除而wlan0仍正常；之前部分“网络无响应”不能认定为内核崩溃。
5. PREEMPT_COUNT不一致曾是猜测，启用后未解决原子发送故障，不能将其写成已确认根因。

## 当前模块与访问

### Qt 自动查找（2026-10-08）

- 默认摄像头为 `luckfox:auto`；点击“打开摄像头”会自动查找，不需要手输地址。
- `luckfoxdiscovery.cpp/h` 使用Qt UDP和当前网卡地址发现设备；优先Wi-Fi，广播无回应时使用本机网段的有限单播查找。超时6.5秒，可取消。
- 板子 `/oem/usr/bin/luckfox-discovery.py` 回应发现请求；`/etc/init.d/S99zzzdiscovery` 开机启动。协议包含随机请求nonce和稳定device_id，RTSP地址采用回应源IP。
- QSettings `LuckfoxTracker/Camera` 保存上次地址和设备标识；旧地址无效时仍能重新发现。上位机实际点击按钮已发现无线地址并显示“RTSP 视频流已连接”。
- 验证：板子软件重启后发现服务自动运行；故意将缓存地址改为192.0.2.1，上位机点击“打开摄像头”仍重新发现10.226.35.30并显示实拍画面。截图 `tmp/luckfox-auto-discovery-live.png`，日志 `tmp/autodiscovery-gui.log`。
- Release已编译，运行文件 `build/Desktop_Qt_6_10_1_MSVC2022_64bit-Release/release/untitled7.exe`。

- `/userdata/wifi-driver/r8188eu.ko` 347700字节。
- `lib80211.ko` 7868、`lib80211_crypt_wep.ko` 5824、`lib80211_crypt_ccmp.ko` 7276字节。
- 主驱动未在crypto.h对齐修复后重编，实际无线视频和首次自动启动已成功。
- 固件 `/lib/firmware/rtlwifi/rtl8188eufw.bin`。
- 真实热点配置 `/userdata/cfg/wpa_supplicant.conf`，禁止打印密码。
- 初始化时临时将kernel modprobe设为`/bin/true`，随后恢复原路径`/sbin/modprobe`。
- ADB `tmp/android-tools/platform-tools/adb.exe`，使用192.168.8.93:5555或当前无线IP:5555；offline时disconnect/connect。
- 视频服务 `rkipc -a /oem/usr/share/iqfiles`。

## 构建与诊断文件

- 持久构建磁盘 `tmp/wifi-build.qcow2`，不要重建、格式化或当文本读取。
- `tmp/start_compile_vm.py`、`tmp/vm_console.py`、`tmp/resume_wifi_build.sh`。
- `tmp/repair_wifi_abi.py`、`tmp/prepare_ccmp.py`、`tmp/build_ccmp.sh`。
- 内核日志工具 `tmp/wifi-klog`，运行180秒、O_SYNC写 `/userdata/wifi-kernel.log`。
- 自动启动验证日志 `tmp/wifi-kernel-after-auto.log`，有CCMP注册和assoc success，无BUG/Oops。
- 手工测试 `D:\miniconda3\python.exe tmp\verify_wifi_rtsp.py <无线IP>`。
- 早期 `tmp/wifi-phase.sh` 未包含AES依赖，不应直接使用。
