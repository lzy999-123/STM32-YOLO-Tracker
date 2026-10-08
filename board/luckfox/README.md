# Luckfox 无线视频部署记录

验证设备：Luckfox Pico Plus，Linux 5.10.160，RTL8188EU/8188ETV USB网卡（0bda:0179）。
这是当前已运行设备的部署脚本和驱动修复记录，不能仅凭相同内核版本认为其他固件的模块ABI也相同。

## 文件与板子路径

| 仓库文件 | 板子路径 |
| --- | --- |
| S99zzzwifi | /etc/init.d/S99zzzwifi |
| S99zzzdiscovery | /etc/init.d/S99zzzdiscovery |
| luckfox-discovery.py | /oem/usr/bin/luckfox-discovery.py |

两个启动脚本需要可执行权限。发现服务使用板子现有的Python 3和UDP 39093；Qt收到回应后连接回应源地址的RTSP `/live/0`。
Wi-Fi脚本读取板子已有的 `/userdata/cfg/wpa_supplicant.conf`；仓库不保存热点密码。
有线管理地址为192.168.8.93/24，无线地址由热点DHCP分配。开机约一分钟完成无线初始化。

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

完整证据摘要见根目录 `WIFI_DRIVER_HANDOFF.md` 和 `MILESTONE_2026-10-08.md`。
