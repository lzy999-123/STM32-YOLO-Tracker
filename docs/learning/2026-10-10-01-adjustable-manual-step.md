# 学习报告：上位机可调手动步长与三段协议扩展

- 日期：2026-10-10（Asia/Shanghai）
- 本次提交主题：手动步长支持0.1°～10°（阶段未提交）
- 修改前基线：上位机`fae9e3166f83c7bb69791ac882ce58a998de21b3`，保留原有未提交修改；下位机本轮修改前HEX SHA256 `4FA91094F71ADF8D55E468B86467BDEDD1B7278156F5BAF59EB310E8DD72D49F`
- 涉及模块：Qt步长输入框、NetworkController、Luckfox转发、UART协议说明、相关Qt/Python测试；仓库外正式下位机main_gimbal.c、gimbal_config.h、README和新HEX

## 1. 本次目标与完成情况

用户询问为什么手动每次只能0.5°，并选择可调范围0.1°～10°、默认0.5°。现场源码确认：Qt把输入框范围设为0.5～0.5并设只读，下位机MANUAL_STEP_DEG也固定0.5；原网络与UART方向命令没有步长字段。

已解锁输入框，设置一位小数、0.1°加减间隔和角度后缀。方向按钮按点击时输入值生成独立manual_step请求；输入改变不会修改已经排队或重发的请求。Luckfox将请求转换为UART TYPE 0x04，STM32将方向码与步长同时入队执行。保留旧单字节CMD的0.5°行为，继续使用原手动模式、使能、急停、故障、校准和目标限位逻辑。

Qt、Luckfox脚本及STM32源码已修改，构建和软件验证通过。Luckfox脚本已实机部署；新版上位机已更新到原Release目录，新HEX已生成。尚未烧录STM32及验证实物步长精度。

## 2. 本次学到的知识

- 参数界面必须接通全链路 → 原输入框即使放开也没有协议字段 → 同时检查界面、发送层、桥接层、执行层。
- 新功能需要明确旧版本行为 → 旧桥接会忽略未知manual_step，旧下位机会忽略TYPE 0x04 → 用独立消息类型让升级不完整时不误执行固定步长。
- 非幂等运动不能靠重发扩大位移 → 一个方向点击有独立request且冻结步长 → 延用网络去重、串口不重发策略。
- 运动目标与实际位移不同 → 软件检查的是命令角度、队列和保护门槛 → 实物精度需要另行测量，ACK只证明收到。

## 3. 遇到的问题与解决过程

| 现象 | 证据与原因 | 处理方法 | 结果 |
| --- | --- | --- | --- |
| 输入框无法加减 | 源码setRange(0.5,0.5)、setReadOnly(true) | 改成0.1～10.0，并在点击时读取、量化为0.1°整数单位 | 实际Qt窗口输入、上下键和方向点击验证通过 |
| 下位机步长固定 | MANUAL_STEP_DEG及旧单字节方向协议 | 增加独立消息manual_step与UART TYPE 0x04，队列保存方向及步长 | 桥接到真实C解析、队列、目标处理检查通过 |
| 旧版本可能误执行0.5° | 若仅给旧command追加字段，旧桥接仍会发送固定方向码 | 使用独立消息类型，禁止自动降级及拆分重复步进 | 无ACK时报告失败，测试确认没有旧命令回退 |
| 首次GUI测试不能启动 | 进程退出C0000135，尚未部署完整Qt运行依赖 | windeployqt与现有运行依赖部署后重新运行 | GUI操作测试通过 |
| 首次部署脚本找不到本地连接文件 | root计算少上一级，尚未执行任何板端修改 | 修正parents索引后重跑 | 板端备份、哈希、语法检查、服务重启和保护配置核对通过 |

## 4. 验证与证据

- Python桥接回归：`python -m unittest discover -s tests -p test_control_bridge.py`，26项通过。覆盖四方向/四种步长、网络重复请求与ACK丢失、非法步长、旧固件无ACK时不回退，以及已有控制逻辑。
- NetworkController回归：7 passed、0 failed、1 skipped（未配置隔离Wi-Fi测试端点）。新增用例确认重试request和步长不变、排队请求保留各自步长、拒绝非法参数。
- 实际Qt窗口操作：`manualButtonsUseEditableStepAndRespectMode`通过，包含键盘输入2.5、上下键2.6/2.5、四方向鼠标点击发送1/5/25/100个0.1°单位，自动模式和离线禁用。
- 与本次关联的既有GUI回归：`mirrorOptionsInvertControlOffsets`、`controlConnectionSurvivesVideoCloseAndRefresh`通过。测试使用本地回环端点，不控制真实电机。
- 临时主机验证`build/manual-step-tests/check_firmware.py`：31项检查通过。提取当前真实C函数，使用真实UART CRC解析、方向/步长队列、运动入口及SetTargets/限位，并接入真实Python Bridge；覆盖四方向×四步长、非法payload、CRC损坏、模式/急停/系统故障/编码器故障/无校准、旧0.5°命令、限位及多步长队列。硬件I/O以桩替代，不能据此证明物理运动或温升。
- 新版Qt Release构建成功，运行依赖已部署；程序SHA256 `D6CDDDDCCC1AAC4049F6444A1E3B7FBA7482A43D660866DA3ED24FEB2C3DAA1A`。构建目录`build/manual-step-Release`；原程序已备份为该目录的`untitled7-before.exe`。
- 原Release目录的新版正式程序已执行一次隐藏窗口启动检查，进程与窗口消息响应正常，并向本次测试进程发送正常关闭请求。该检查没有点击运动按钮或打开私人摄像头，不据此宣称无线端到端可调步长已验收。
- Keil正式目标servo_duoji构建成功，0 errors / 0 warnings，Code=31610、RO-data=6046、RW-data=140、ZI-data=3068；构建日志`build/manual-step-Release/keil-build.log`。
- 新HEX：`D:\STM32CubeMx\hal_stm32\科教协同下位机-1\MDK-ARM\servo_duoji\servo_duoji.hex`；SHA256 `412D12ED3B2EEE3427D690403178F9FDD077D2A62B8E99133558EF2BE4D3739D`。
- 下位机改前源码、配置、README及HEX备份在本机临时目录`gimbal-manual-step-20261010-before`。本次只修改正式下位机main_gimbal.c、gimbal_config.h与README，未修改电角度校准、Flash记录格式/版本、控制增益、电压或保护阈值。
- 重新发现真实Luckfox地址后通过现有连接部署`/oem/usr/bin/luckfox-control.py`；板端原脚本备份`/oem/usr/bin/luckfox-control.py.20261010-manual-step-7c96d8c6.bak`。新脚本MD5 `cce0ad034320b54b83918c38b4e7b145`与本地一致，语法检查、控制服务重启与status通过；控制配置、视频配置部署前后MD5相同。明细`build/manual-step-Release/bridge-deployment.json`。
- 本次未烧录、请求校准或发送真实运动命令，未创建Git提交或推送。保留上位机及下位机此前的OLED等修改，未将私人截图、连接凭据或本地诊断脚本加入仓库。

## 5. 以后做项目要注意什么

界面参数改动前逐层核对协议及下位机单位，采用整数单位避免跨语言浮点歧义。对不可重复执行的运动请求冻结参数并去重。先部署依赖再验证GUI；升级多段链路时记录每段部署状态，不能把软件构建说成实体更新完成。

## 6. 当前不足与下一步

用户需要将新HEX下载到STM32后，先用0.5°进行四方向检查，再检查0.1°和较大步长。核对实际方向、遥测变化、回中与故障状态；实际反馈会受机械负载和控制误差影响，尚无物理位移精度或长期温升数据。本次保留校准记录格式，但若下载操作执行全片擦除，会删除原Flash校准记录。整个项目仍未宣称完成。

## 7. 对最终项目总结的贡献

记录从界面固定数值追溯到三段协议限制的过程，提供参数冻结、旧版本不误执行、端点独立校验及软件/物理验收区分的实现与证据。
