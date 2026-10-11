# 学习报告：时间显示移到画面左上角

- 日期：2026-10-10（Asia/Shanghai）
- 本次提交主题：按用户要求调整视频时间位置（阶段未提交）
- 修改前基线：`fae9e3166f83c7bb69791ac882ce58a998de21b3`加已有未提交工作；改前EXE SHA256 `DE1ABF3F58F82F6AC3A2388D3DA0D9E1B318166C62645B602500D1F5A6EC1644`
- 涉及模块：mainwindow.cpp、既有时间显示回归、操作说明与学习报告索引

## 1. 本次目标与完成情况

用户要求时间位于画面左上角。将时间正文和黑色阴影的绘制对齐从底部改为顶部，继续以实际视频区域为定位基准，距边缘12像素。已构建并更新原Release目录程序，构建与交付EXE哈希一致。

## 2. 本次学到的知识

- 显示文字定位 → 时间应相对视频区域定位，不应锚定窗口边角 → 保持画面缩放和黑边下的位置一致。
- 正文与阴影同步 → 两次drawText均改为AlignTop → 避免阴影与文字分离。

## 3. 遇到的问题与解决过程

| 现象 | 证据与原因 | 处理方法 | 结果 |
| --- | --- | --- | --- |
| 时间原来位于左下角 | 两次drawText使用AlignBottom | 改为AlignTop，更新原位置回归 | 真实Qt渲染的左上角时间像素检查通过 |

## 4. 验证与证据

- 应用与测试构建均退出0：`build/manual-step-Release/clock-top-left-build.log`、`build/manual-step-tests/clock-top-left-build.log`。
- 既有`displayedClockDoesNotModifyTrackingFrames`回归改为检查视频左上角，正常、垂直翻转、旋转180°三种模式均检查到文字，同时确认输入跟踪的图像未被时间文字修改；含初始化/收尾共3 passed、0 failed、0 skipped。日志：`build/manual-step-tests/clock-top-left-results.txt`。
- 使用offscreen Qt和合成黑色720p帧，没有采集私人视频或发送电机指令；本轮没有新增测试套件。
- 确认上位机进程不存在后替换原目录EXE，源与目标SHA256均为`790978F6D40456FC3E322DA1ED9C201EAEC72CB9EF5DFEC5B2C950B7474018FA`。
- 改前源码、既有测试、说明与EXE备份于被忽略的`build/clock-top-left-before/`；`git diff --check`通过。

## 5. 以后做项目要注意什么

文字在完成方向校正后的显示画布绘制，位置跟随视频区域；调整位置后同步检查阴影、说明和既有位置断言。

## 6. 当前不足与下一步

用户打开更新后的上位机即可查看左上角时间。本次完成软件渲染检查，未打开实体摄像头复看位置；未提交推送。

## 7. 对最终项目总结的贡献

补充[报告05](2026-10-10-05-upright-display-time-overlay.md)中时间显示的后续位置调整，记录图像输入与显示文字分离的回归结果。
