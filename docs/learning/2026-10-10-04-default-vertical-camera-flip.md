# 学习报告：默认垂直翻转画面

- 日期：2026-10-10（Asia/Shanghai）
- 本次提交主题：按实际摄像头安装方向设置上位机默认画面（阶段未提交）
- 修改前基线：`fae9e3166f83c7bb69791ac882ce58a998de21b3`加已有未提交工作；改前上位机EXE SHA256 `BD6F791FEFC894C50893A1B442B0012C4FD4BA38F0D7977BBF2EE580A986E219`
- 涉及模块：mainwindow.cpp、现有方向/控制窗口测试、操作说明和报告索引

## 1. 本次目标与完成情况

用户说明当前摄像头安装方向下，手动选择垂直翻转才是正常画面。上位机现在每次启动默认设置MirrorV，沿用既有垂直翻转显示和控制偏移处理；仍可在底部画面方向菜单选择其他模式，菜单勾选当前选项。

RTSP帧在CameraManager中翻转，本地QVideoFrame在MainWindow中翻转；本轮设置共享方向状态，未增加第二次翻转。切换方向继续停止旧跟踪和清空旧偏移。源码与测试构建成功，相关实际Qt对象/窗口回归通过，新EXE已更新到原Release目录。

## 2. 本次学到的知识

- 摄像头安装默认值应明确设置 → 用户每次都选择垂直翻转 → 启动时使用既有模式，可以同时覆盖网络和本地画面路径。
- 展示设置要反映当前状态 → 原方向菜单无法看出选中项 → 使用互斥可勾选菜单，便于确认默认值和手动切换结果。
- 图像方向与跟踪坐标共同处理 → 既有MirrorV会在发送控制偏移时反转Y → 只改变默认选择，保持与用户原来手动选择相同的控制处理，不未经实物验证修改电机方向。

## 3. 遇到的问题与解决过程

| 现象 | 证据与原因 | 处理方法 | 结果 |
| --- | --- | --- | --- |
| 每次打开都要手动垂直翻转 | CameraManager原默认Normal，MainWindow没有设置安装方向 | 主窗口初始化时设置MirrorV | 默认模式与原手动MirrorV控制偏移检查通过 |
| 方向菜单不显示选中项 | 原动作未设置checkable或互斥组 | 增加QActionGroup，按当前模式勾选 | 默认垂直勾选、切换正常后垂直取消勾选验证通过 |
| 测试重建时目标EXE被占用 | 在测试链接完成前过早启动测试EXE，链接报告LNK1104 | 等测试退出后完成重建，再顺序执行 | 最终构建及检查退出0；不把第一次失败写成通过 |

## 4. 验证与证据

- Qt Release构建成功，日志`build/manual-step-Release/vertical-default-build.log`。
- 测试最终构建成功，日志`build/manual-step-tests/vertical-default-build-final.log`。
- 现有`mirrorOptionsInvertControlOffsets`回归新增默认MirrorV、菜单勾选/互斥切换、清除旧偏移检查，并继续验证各方向的原控制转换；`controlConnectionSurvivesVideoCloseAndRefresh`验证关闭/刷新仍保留控制连接。两项加初始化/收尾共4 passed、0 failed、0 skipped；日志`build/manual-step-tests/vertical-default-test-results.txt`。
- 测试使用offscreen Qt平台与回环模拟服务，没有打开私人摄像头或发送实体运动指令；尚未实机查看本次默认启动画面和自动跟踪方向，不将其写成实物验收通过。
- 旧源码、测试源码及EXE备份于被忽略的`build/vertical-default-before/`。更新前发现用户旧程序运行，最终检查时已经退出，没有由助手关闭它；随后复制新EXE到原Release目录，并核对构建源与目标哈希一致。
- 新EXE：`build/Desktop_Qt_6_10_1_MSVC2022_64bit-Release/release/untitled7.exe`；SHA256 `43D06A02290B4278C0AB04054E83EAAE5425446B2F3DBBE4FCBB831A806E10E0`。
- 本轮未修改STM32或Luckfox脚本，没有烧录、重启或提交推送；此前下位机坐标修正版的实物验收仍按[报告03](2026-10-10-03-preserve-angle-origin-and-home-targets.md)待确认。

## 5. 以后做项目要注意什么

默认画面应和用户实际安装方向对应。复用已有变换入口，避免显示和跟踪对同一帧重复翻转。方向变更时清除旧目标结果，菜单显示当前选择。测试构建与启动必须顺序进行，不能同时占用待链接的EXE。

## 6. 当前不足与下一步

重新打开更新后的上位机，点击打开摄像头，确认无需手动选择即可显示用户正常画面。显示与控制软件行为等同于此前手动选择MirrorV；物理跟踪方向尚未重新实测。本次没有新增方向设置持久化，每次启动均默认MirrorV。

## 7. 对最终项目总结的贡献

补充实际安装方向对默认显示和跟踪偏移转换的要求，记录共用方向状态、菜单状态和运行中变更的处理；区分窗口软件检查与摄像头/电机实物验收。
