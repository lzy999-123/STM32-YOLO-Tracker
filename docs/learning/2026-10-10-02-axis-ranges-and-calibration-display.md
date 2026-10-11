# 学习报告：独立轴角度坐标、行程与校准参考显示

- 日期：2026-10-10（Asia/Shanghai）
- 本次提交主题：水平/垂直行程与校准参考角度显示（阶段未提交）
- 修改前基线：上位机`fae9e3166f83c7bb69791ac882ce58a998de21b3`加已有未提交工作；承接[报告01](2026-10-10-01-adjustable-manual-step.md)；下位机改前HEX SHA256 `412D12ED3B2EEE3427D690403178F9FDD077D2A62B8E99133558EF2BE4D3739D`
- 涉及模块：用户新增UI标签、Qt实时角度/校准信息显示、NetworkController、Luckfox 0x83解析、仓库外gimbal_config.h、gimbal_foc.c/.h、main_gimbal.c及说明

## 1. 本次目标与完成情况

用户明确水平行程−60°～+70°、垂直行程+140°～+235°，校准姿态赋值水平+40°、垂直+200°，并提供新增“上电时校验角度”控件截图。

已接入用户保存的label_16和label_17，保留新增控件位置与整体布局。下位机给两轴分别配置逻辑参考坐标，以各自参考计算目标及实测值；回中使用(+40°, +200°)。手动和自动运动目标都沿用SetTargets限位。普通上电保持电机禁用，保持原校准电角度、机械记录格式/版本和保护阈值。

下位机新增UART 0x83定期报告参考坐标、行程及有效标志，Luckfox转入status.calibration，Qt显示实际报告；未知、过期、校准无效或离线显示“--”。实时角度在状态栏保留符号，不再按360°取模。

Qt和STM32构建、软件检查和真实Qt窗口操作通过，新上位机程序已更新到原Release目录，新HEX已生成。用户重启Luckfox后，新桥接脚本已通过ADB完成部署，服务/UART启动、脚本哈希与配置保护检查通过；真实网络握手及在线遥测读取通过。实体STM32烧录及实际机械行程验收尚未执行。

## 2. 本次学到的知识

- 两轴参考不同不能共用中心常量 → 水平40°和垂直200°需要独立logical_center_deg → 在轴对象中维护坐标变换参数，避免在显示层加固定偏移掩盖控制错误。
- 限位目标和实测值分别处理 → SetLogicalTarget限幅、GetLogicalAngle保持连续反馈 → 实物越界时仍能看见真实测量，而不会把读数钉在边界。
- 配置显示需要设备证据 → 新界面标签显示0x83实际报告而非硬编码40/200 → 未升级下位机时显示未知，避免假装已生效。
- 旧机械校准和新逻辑坐标是不同信息 → Flash保存编码器机械参考和电角度，本次逻辑参考在固件轴配置中 → 机械姿态不变时可以继续加载原记录。

## 3. 遇到的问题与解决过程

| 现象 | 证据与原因 | 处理方法 | 结果 |
| --- | --- | --- | --- |
| 两轴使用统一0°中心 | Configure/Calibrate/SetLogicalTarget/GetLogicalAngle和CenterGimbal共用常量 | 增加每轴参考坐标及Configure参数，更新所有生产调用 | 构建及实际C坐标/回中检查通过 |
| 水平负角度变成300° | 既有normalizeAngle360显示逻辑 | 按有符号一位小数显示实测值 | Qt窗口−60°显示用例通过 |
| 新标签只是“水平/竖直”占位 | 用户UI新增label_16/17没有数据绑定 | 设备新增校准报告、桥接解析、Qt信号绑定 | 有效40/200、缺失和离线“--”验证通过 |
| 调整布局后地址挡住刷新按钮 | 相关既有GUI回归发现label_9与pushButton_8重叠 | label_9宽度190缩至150，不改变用户新增控件坐标 | 重新构建与GUI回归通过 |
| 首次过期测试收不到status | 测试直接前进2.6秒同时使1秒客户端租约过期 | 保持心跳和遥测，单独等待校准报告过期 | 28项Python回归通过，未修改租约机制 |
| 板端部署语法检查超时 | 新脚本上传、哈希核对后SSH读取超时；后续SSH握手与ADB shell也不响应 | 用户重启后通过ADB确认旧活动脚本，先停止控制服务释放内存，再用内置compile检查并原子切换；保留备份和失败回退 | 新脚本部署成功；重启后可用内存约4MB、停服务后约8MB是观察，未确认此前卡住的原因 |

## 4. 验证与证据

- Python转发回归：28项全部通过，包含0x83真实CRC帧解析、参考与行程、有/无校准、非法mask/长度/范围及独立过期检查。
- Qt网络回归：8 passed / 0 failed / 1 skipped；跳过未配置隔离Wi-Fi测试端点的既有用例。校准对象校验、状态序号防倒退、旧消息缺失处理通过。
- 实际Qt窗口：校准显示、可调步长、关闭视频和刷新保留控制三项用例通过，连同初始化/收尾为5 passed / 0 failed。新用例确认显示+40.0°和+200.0°、实时−60.0°/+235.0°、移动后参考不变、缺失/离线清空、标签宽度和窗口缩放布局。
- Qt测试使用回环模拟控制端，未打开私人摄像头；本地空画面截图`build/manual-step-tests/angle-profile-ui.png`用于布局查看，不加入仓库。
- `build/manual-step-tests/check_angles.py`：50项主机软件检查通过，提取当前真实C函数验证四方向四步长、坐标映射、新范围上下边界、回中、未裁剪的超范围实测值、Flash校准应用保持禁用、0x83实际打包和Python转发。硬件读取/输出以桩替代，不是实体角度精度或温升测试。
- Qt Release最终构建成功；原Release目录和构建目录程序SHA256均为`BD6F791FEFC894C50893A1B442B0012C4FD4BA38F0D7977BBF2EE580A986E219`。原目录`build/Desktop_Qt_6_10_1_MSVC2022_64bit-Release/release/untitled7.exe`已经更新。
- Keil正式目标构建成功，0 errors / 0 warnings，Code=31838、RO-data=6046、RW-data=144、ZI-data=3072；日志`build/manual-step-Release/angle-keil-build.log`。
- 新HEX：`D:\STM32CubeMx\hal_stm32\科教协同下位机-1\MDK-ARM\servo_duoji\servo_duoji.hex`；SHA256 `F9C5A74485C1D95C133AA3F2E4F93B0B8671567F2BA056650B4F381F29BC5920`。
- 改前Qt源码、用户已保存UI、原程序和下位机源码/HEX快照位于`build/angle-profile-before/`。原`gimbal_calibration.c/.h`Flash结构和版本没有修改。
- Luckfox部署：用户确认已重启，ADB shell恢复，boot ID为`3a16a3c8-59cf-4bc4-be42-e6d4b74efe3a`。活动脚本MD5由`cce0ad034320b54b83918c38b4e7b145`更新为与本地一致的`e9fd2345deec4b6b0cf20a6ed108fdda`；旧脚本备份为`/oem/usr/bin/luckfox-control.py.20261010-angle-profile-retry-2dccf39a.bak`。
- 停服务后内置`compile`检查通过，原子替换后`S99zzzcontrol`启动及状态正常，Python子进程1133存在并运行目标脚本，日志确认`/dev/ttyS2`已打开。`/userdata/cfg/luckfox-control.json` MD5 `d6a186abb5a401e347ad93d17b0e0ff2`与`/userdata/rkipc.ini` MD5 `96b5f4262414af539bcd4660986e109e`部署前后均未变化；证据保存于被忽略的`build/manual-step-Release/angle-bridge-deployment.json`。
- 实际UDP会话读取：收到welcome及10个status，全部online=true、seq递增，最后实时角度38.6°/176.8°、mode=0、flags=0；新字段`calibration=null`，没有收到有效新参考报告，不能写成实体40°/200°显示已验收。仅发送hello/heartbeat/bye会话消息，无步进、回中、切换模式、跟踪目标或校准请求；bye按既有协议释放会话并停止跟踪。证据`build/manual-step-Release/angle-bridge-real-status.json`，未打开摄像头。
- `git diff --check`通过，只有现有LF/CRLF提示。没有提交或推送，没有实体烧录、校准、发送真实运动命令或执行板端重启。

## 5. 以后做项目要注意什么

同时核对控制坐标、目标限位、回中、遥测和显示，不能只改界面角度。通过专门报告区分校准参考与当前实测角度，并让失效信息变为未知。用户调整UI后运行相关按钮回归，及时发现透明标签也可能遮挡点击。

## 6. 当前不足与下一步

需要烧录本次新HEX并完成真实设备报告/步长/回中/行程验证。只改变逻辑数字且机械参考未变时无需为此重新校准；若改变实际机械参考，仍需按既有流程校准。运行目标范围约束不能作为FOC电角度校准扫描或实物不越界的保证，应保留校准扫描空间。

板端新脚本已部署并核对服务、哈希及真实在线遥测。目前没有有效新参考报告，界面显示“--”符合预期。烧录新STM32固件后仍需确认0x83实际报告和新参考显示，并完成实物步长、回中及行程验收；本次没有进行视频验收，也没有把端口开放作为控制成功的证据。

## 7. 对最终项目总结的贡献

记录独立轴参考坐标、非对称行程和有来源的校准信息显示，补充旧机械校准应用与新逻辑坐标的兼容方法；保持代码/软件验收、板端部署与实体运动验收的边界。
