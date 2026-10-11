# 学习报告：翻转画面后的正向时间显示

- 日期：2026-10-10（Asia/Shanghai）
- 本次提交主题：修复垂直翻转后时间水印倒置（阶段未提交）
- 修改前基线：`fae9e3166f83c7bb69791ac882ce58a998de21b3`加已有未提交工作；承接[报告04](2026-10-10-04-default-vertical-camera-flip.md)；改前上位机EXE SHA256 `43D06A02290B4278C0AB04054E83EAAE5425446B2F3DBBE4FCBB831A806E10E0`
- 涉及模块：mainwindow.cpp绘制、Qt窗口检查、Luckfox时间OSD配置、操作说明和报告索引

## 1. 本次目标与完成情况

用户提供截图：垂直翻转后人物正常，左下角时间文字仍倒置。板端实际配置确认`[osd.1] type=dateTime, enabled=1`，原时间已由Luckfox写入视频，所以上位机翻转整帧时文字也被翻转。

已关闭板端原时间OSD，上位机在方向变换和画面缩放完成后，用QPainter在实际视频区域左下角绘制正向日期时间。使用电脑本地当前时间，格式`yyyy-MM-dd HH:mm:ss`，黄色字加黑色阴影；缩放留黑边时仍定位在视频内。绘制只写显示画布，不写cv::Mat，因此时间文字不会进入目标跟踪图像。

本次已构建、更新原Release程序，板端配置已部署并持久核对，真实视频恢复及实际Qt开关/刷新流程通过。助手未启动目标跟踪或发送实物移动目标，未修改下位机或控制脚本。没有保存用户私人视频画面。

## 2. 本次学到的知识

- 嵌入视频的OSD与上位机图层不同 → 板端时间会随cv::flip一起倒置，FPS和跟踪框由上位机画所以仍正向 → 与观看方向无关的文字应在图像方向校正后绘制。
- 显示图层与跟踪输入分开 → 使用QPainter而非向cv::Mat写时间 → 避免让水印成为特征跟踪或检测的输入。
- 配置修改要等待进程完全退出 → rkipc退出期间可能将旧内存配置保存回文件 → 必须确认旧PID消失，再替换文件、启动单一新进程并再次核对。
- 旧ADB的shell退出状态不能直接信任 → 客户端返回0不代表远程条件命令成功 → 加远程退出状态标记，并用实际PID和配置内容核对。

## 3. 遇到的问题与解决过程

| 现象 | 证据与原因 | 处理方法 | 结果 |
| --- | --- | --- | --- |
| 画面正常但时间倒置 | 用户截图、板端dateTime OSD开启，上位机翻转整帧 | 关闭板端时间，改为方向变换后绘制 | 合成画面检查及真实视频流程通过，用户显示待重新打开查看 |
| ADB exec-out不可用 | 该板端返回error: closed，尚未修改配置 | 使用ADB pull读取原始文件到被忽略目录 | 读取成功，没有输出完整配置内容 |
| nohup后台启动未生效 | 没有新视频PID，也没有日志文件；具体shell退出机制未进一步确认 | 回退配置，改用start-stop-daemon及独立PID文件启动 | 视频进程恢复 |
| 曾出现两个rkipc PID及配置被覆盖 | 旧进程退出超过原等待窗口；旧ADB未向客户端传递远程失败状态，随后旧进程晚写配置 | 明确等待旧PID消失，校验远程退出状态，检测关闭时参数变化并回退 | 最终单一PID1755、时间OSD为0，后续核对稳定 |

## 4. 验证与证据

- Qt Release构建成功，日志`build/manual-step-Release/upright-clock-build.log`；测试构建成功，日志`build/manual-step-tests/upright-clock-build.log`。
- 合成黑色720p帧的实际Qt绘制检查`displayedClockDoesNotModifyTrackingFrames`通过：正常、垂直翻转、180°三种状态在视频底部绘制黄色文字，m_lastFrame与输入像素保持相同。该检查验证图层和输入分离，不是字符OCR验收。
- 同时运行现有默认方向/控制偏移及关闭视频/刷新保留控制检查，三项加初始化和收尾共5 passed、0 failed、0 skipped；日志`build/manual-step-tests/upright-clock-test-results.txt`。
- 板端只改变`osd.1.enabled: 1 -> 0`；所有其他INI参数解析值保持不变。配置MD5由`96b5f4262414af539bcd4660986e109e`变为`124d8d542d067442e55d18a720b781ed`；成功部署备份`/userdata/rkipc.ini.20261010-upright-clock-a84478ec.bak`。记录`build/manual-step-Release/upright-clock-board-deployment.json`。
- `/userdata/cfg/luckfox-control.json` MD5仍为`d6a186abb5a401e347ad93d17b0e0ff2`，活动控制脚本MD5仍为`e9fd2345deec4b6b0cf20a6ed108fdda`；控制守护/子进程PID814/816保持不变。没有重启控制服务或整板。
- 真实设备Qt窗口检查`liveDeviceStartupVideoAndRefresh`通过：启动控制在线但视频未自动打开、点击打开收到1280×720帧、关闭视频控制仍在线、刷新发现成功且视频保持关闭、重新打开收到视频。加初始化/收尾共3 passed、0 failed、0 skipped；日志`build/manual-step-tests/upright-clock-live-results.txt`。没有录制或保存私人画面，没有进行物理跟踪方向验收。
- 最新原Release程序`build/Desktop_Qt_6_10_1_MSVC2022_64bit-Release/release/untitled7.exe`已更新，并与构建源哈希一致；SHA256 `6CB5D3EF674123F370F22F6BF4D05A7BE9DEA62A09D7A6A88CAB4FE5C9B080E2`。
- 修改前源码、测试和EXE备份于`build/upright-clock-before/`；板端INI和本地诊断材料均位于被忽略目录，未加入仓库。未复制用户截图，未提交或推送。

## 5. 以后做项目要注意什么

画面方向变更时分别检查视频像素、板端OSD和上位机文字图层。展示文字应限制在视频区域，不写跟踪输入。修改设备配置后核对运行状态及持久化文件，不能把启动命令返回或短时哈希正确当作最终生效证据。

## 6. 当前不足与下一步

用户重新打开新版上位机查看正向时间。时间来自电脑显示时刻，不是摄像头曝光/采集时刻，不能用它测视频端到端延迟。原始RTSP码流已不包含板端时间文字，其他播放客户端不会自动得到这层上位机时钟。本次检查包含真实视频接收与窗口流程，但未对私人画面保存截图或执行字符识别。

## 7. 对最终项目总结的贡献

补充安装方向校正与文字图层的处理，记录显示时间与采集时间的区别，以及资源受限板端的进程退出、可靠启动和持久配置核对经验。
