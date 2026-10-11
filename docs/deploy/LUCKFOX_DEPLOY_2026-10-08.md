# Luckfox 板端脚本部署任务书（对应提交 fae9e31）

本文给执行代理（Codex）使用。请按顺序执行，每一步检查“预期结果”。不符合预期时**立即停止**，
把命令和完整输出报告给用户，不要自行猜测修复，不要跳步。

## 0. 约束

- 工作目录：`D:\QT\project\untitled7`（Git Bash）。
- ADB：`tmp/android-tools/platform-tools/adb.exe`，下文记作 `$ADB`。
- 板子地址：
  - 有线 `192.168.8.93:5555`；不通时向用户询问当前无线 IP，不要扫描网络。
  - 下文记作 `$DEV`。如果只连了一台设备，可以省略 `-s $DEV`。
- **本任务不启用认证**：不生成、不写入 `auth_key`，不创建 `luckfox-control.key`。
- **不修改**：
  - `/userdata/cfg/luckfox-control.json`（其中的 UART 配置已经实测确认）；
  - `/userdata/cfg/wpa_supplicant.conf`；
  - `S99zzzwifi`。
- **不关闭 ADB**：用户目前依赖它部署板子。
- 不把密码、热点信息、私人截图写进仓库或报告。
- 本任务**只部署 Luckfox 脚本**，不烧录 STM32 固件。顺序必须是先部署本文脚本，再由用户另行烧录固件。
  原因：旧桥接只接收 5 字节遥测，新固件发 6 字节，会被旧桥接丢弃。

## 1. 部署前检查（本地）

```bash
cd /d/QT/project/untitled7
ADB=tmp/android-tools/platform-tools/adb.exe
DEV=192.168.8.93:5555
git log --oneline -1                     # 预期：HEAD 包含 fae9e31 或更新的提交
git status --short board/luckfox         # 预期：无输出（脚本没有本地未提交改动）
file board/luckfox/S99zzzcontrol board/luckfox/S99zzzdiscovery board/luckfox/*.py
                                         # 预期：都不含 "CRLF"
python -m unittest discover -s tests -p test_control_bridge.py
                                         # 预期：OK（22 项）
```

## 2. 连接板子

```bash
$ADB connect $DEV
$ADB -s $DEV shell "uname -a; cat /userdata/cfg/luckfox-control.json"
```

预期结果：

- 连接成功，内核为 Linux 5.10.160；
- 配置文件存在，且含 `"uart": "/dev/ttyS2"`。

如果配置文件不存在，或 UART 不是 ttyS2，停止并报告。

## 3. 用旧脚本停止服务（必须在覆盖文件之前做）

新旧启动脚本的 PID 文件格式不同：旧版记录 Python 进程号，新版记录守护外壳的进程号。
先覆盖文件再 stop，会留下占用 UDP 5005 的旧进程，导致新服务反复启动失败。

```bash
$ADB -s $DEV shell "/etc/init.d/S99zzzcontrol stop; /etc/init.d/S99zzzdiscovery stop; sleep 1"
$ADB -s $DEV shell "ps | grep -E 'luckfox-(control|discovery)' | grep -v grep"
```

预期结果：第二条命令没有输出。如果仍有进程：

1. 记录这些进程的 PID；
2. 执行 `$ADB -s $DEV shell "kill <PID>"`，再检查一次；
3. 仍然存在就停止并报告。

## 4. 备份旧文件

```bash
$ADB -s $DEV shell "mkdir -p /userdata/backup-20261008 && cp /etc/init.d/S99zzzcontrol /etc/init.d/S99zzzdiscovery /oem/usr/bin/luckfox-control.py /oem/usr/bin/luckfox-discovery.py /userdata/backup-20261008/ && ls -l /userdata/backup-20261008/"
```

预期结果：列出 4 个文件。

## 5. 推送新文件

```bash
$ADB -s $DEV push board/luckfox/luckfox-control.py   /oem/usr/bin/luckfox-control.py
$ADB -s $DEV push board/luckfox/luckfox-discovery.py /oem/usr/bin/luckfox-discovery.py
$ADB -s $DEV push board/luckfox/S99zzzcontrol        /etc/init.d/S99zzzcontrol
$ADB -s $DEV push board/luckfox/S99zzzdiscovery      /etc/init.d/S99zzzdiscovery
$ADB -s $DEV shell "chmod +x /etc/init.d/S99zzzcontrol /etc/init.d/S99zzzdiscovery; sync"
```

如果出现 Read-only file system 之类的错误：停止，执行第 9 节回滚，然后报告。

推送后校验文件一致：

```bash
md5sum board/luckfox/luckfox-control.py board/luckfox/luckfox-discovery.py board/luckfox/S99zzzcontrol board/luckfox/S99zzzdiscovery
$ADB -s $DEV shell "md5sum /oem/usr/bin/luckfox-control.py /oem/usr/bin/luckfox-discovery.py /etc/init.d/S99zzzcontrol /etc/init.d/S99zzzdiscovery"
```

预期结果：两边的 md5 逐一相同。

## 6. 板端环境检查

```bash
$ADB -s $DEV shell "head -1 /etc/init.d/S99zzzcontrol | od -c | head -2"
$ADB -s $DEV shell "python3 -S -c 'import hmac,hashlib,json,socket,select;print(\"py-ok\")'"
$ADB -s $DEV shell "sh -n /etc/init.d/S99zzzcontrol && sh -n /etc/init.d/S99zzzdiscovery && echo sh-ok"
$ADB -s $DEV shell "which readlink tr grep"
```

预期结果：

- `od` 的输出中不含 `\r`；
- 输出 `py-ok` 和 `sh-ok`；
- 三个命令都能找到路径。

任何一项不符合，执行第 9 节回滚并报告。

## 7. 启动与检查

```bash
$ADB -s $DEV shell "/etc/init.d/S99zzzcontrol start; /etc/init.d/S99zzzdiscovery start; sleep 3"
$ADB -s $DEV shell "/etc/init.d/S99zzzcontrol status; /etc/init.d/S99zzzdiscovery status"
$ADB -s $DEV shell "tail -30 /tmp/luckfox-control.log; tail -15 /tmp/luckfox-discovery.log"
$ADB -s $DEV shell "netstat -anu 2>/dev/null | grep -E ':5005|:39093'"
```

预期结果：

- 两个服务都显示“正在运行”；
- 日志中没有 Traceback 反复出现；
- 控制日志中出现未配置 auth_key 的警告。这是预期的，表示当前为不认证模式；
- UDP 5005 和 39093 处于监听状态。

## 8. 功能验证

1. **守护重启**：
   ```bash
   $ADB -s $DEV shell "kill \$(cat /var/run/luckfox-control.child.pid); sleep 4; /etc/init.d/S99zzzcontrol status; cat /var/run/luckfox-control.child.pid"
   ```
   预期：仍然显示正在运行，子进程 PID 与 kill 之前不同。
2. **stop 能停干净**：
   ```bash
   $ADB -s $DEV shell "/etc/init.d/S99zzzcontrol stop; sleep 1; ps | grep luckfox-control | grep -v grep; /etc/init.d/S99zzzcontrol start"
   ```
   预期：stop 之后 ps 没有输出，start 之后恢复运行。
3. **重启板子后自启**（需要先征得用户同意再执行）：
   ```bash
   $ADB -s $DEV reboot
   ```
   等待约 90 秒后重新执行 `$ADB connect`，再重复第 7 节的 status 检查。
4. **电脑端联调**（交给用户操作并记录结果，执行代理不要代替用户声称已通过）：
   - 运行新编译的 `build/review-fix-Release/release/untitled7.exe`。
     运行前需要把 ONNX Runtime 1.20.1 的 DLL 放到 exe 同目录。
   - 应能发现设备，云台显示在线；手动方向、模式切换都应收到 ACK。

## 9. 回滚

```bash
$ADB -s $DEV shell "/etc/init.d/S99zzzcontrol stop; /etc/init.d/S99zzzdiscovery stop"
$ADB -s $DEV shell "ps | grep -E 'luckfox-(control|discovery)' | grep -v grep"   # 有残留就 kill 对应 PID
$ADB -s $DEV shell "cp /userdata/backup-20261008/S99zzzcontrol /userdata/backup-20261008/S99zzzdiscovery /etc/init.d/ && cp /userdata/backup-20261008/luckfox-control.py /userdata/backup-20261008/luckfox-discovery.py /oem/usr/bin/ && sync"
$ADB -s $DEV shell "/etc/init.d/S99zzzcontrol start; /etc/init.d/S99zzzdiscovery start; /etc/init.d/S99zzzcontrol status"
```

## 10. 完成后汇报

向用户报告以下内容：

- 每一步是否符合预期，异常时附上原始输出；
- 两边的 md5 是否一致；
- 两个服务的 status；
- 控制日志最后 30 行，**先确认其中不含任何密钥**；
- 守护重启和板子重启的结果；
- 哪些步骤没有执行（例如未经用户同意，没有重启板子）。

如果按照 AGENTS.md 需要提交部署记录，学习报告中只能写实际观察到的结果，并写明基线提交 fae9e31。
