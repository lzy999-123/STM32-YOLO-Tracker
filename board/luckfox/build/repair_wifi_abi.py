from pathlib import Path
import re
import subprocess
import os

os.chdir('/work/kernel')
header = Path('include/linux/skbuff.h')
backup = header.with_suffix('.h.wifi-before')
if not backup.exists():
    backup.write_bytes(header.read_bytes())
text = backup.read_text()
header.write_text(text)
subprocess.run(['scripts/config', '--disable', 'NETFILTER', '--enable', 'IPV6', '--disable', 'XFRM', '--disable', 'SKB_EXTENSIONS'], check=True)
subprocess.run(['scripts/config', '--disable', 'DEBUG_ATOMIC_SLEEP'], check=True)
preempt_kconfig = Path('kernel/Kconfig.preempt')
config_text = preempt_kconfig.read_text()
if 'config PREEMPT_COUNT\n       bool\n       default y' not in config_text:
    config_text = config_text.replace('config PREEMPT_COUNT\n       bool', 'config PREEMPT_COUNT\n       bool\n       default y')
    preempt_kconfig.write_text(config_text)
env = dict(os.environ, ARCH='arm', CROSS_COMPILE='/work/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-', KCFLAGS='-g0')
subprocess.run(['make', 'olddefconfig'], env=env, check=True)
assert 'CONFIG_PREEMPT_COUNT=y' in Path('.config').read_text(), 'live kernel requires preemption accounting'
subprocess.run(['make', '-j2', 'modules_prepare'], env=env, check=True)
probe = Path('wifi-probe')
probe.mkdir(exist_ok=True)
(probe / 'Makefile').write_text('obj-m += probe.o\n')
(probe / 'probe.c').write_text('#include <linux/skbuff.h>\n#include <linux/netdevice.h>\n#include <linux/module.h>\nint wifi_len(void){return offsetof(struct sk_buff,len);}\nint wifi_data(void){return offsetof(struct sk_buff,data);}\nint wifi_devaddr(void){return offsetof(struct net_device,dev_addr);}\nMODULE_LICENSE("GPL");\n')
subprocess.run(['make', '-j2', 'M=wifi-probe', 'modules'], env=env, check=True)
layout = subprocess.check_output(['/work/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-objdump', '-d', 'wifi-probe/probe.o'], text=True)
print(layout, flush=True)
assert re.search(r'<wifi_len>:[\s\S]*?#80\b', layout), 'skb len offset does not match live kernel'
assert re.search(r'<wifi_data>:[\s\S]*?#164\b', layout), 'skb data offset does not match live kernel'
assert re.search(r'<wifi_devaddr>:[\s\S]*?#460\b', layout), 'netdev address offset does not match live kernel'
subprocess.run(['make', '-j2', 'M=drivers/staging/rtl8188eu', 'modules'], env=env, check=True)
Path('/work/r8188eu.ko').write_bytes(Path('drivers/staging/rtl8188eu/r8188eu.ko').read_bytes())
subprocess.run(['sh', '/work/build_dependencies.sh'], env=env, check=True)
subprocess.run(['/work/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-strip', '--strip-debug', '/work/r8188eu.ko', '/work/lib80211.ko', '/work/lib80211_crypt_wep.ko', '/work/lib80211_crypt_ccmp.ko'], check=True)
subprocess.run(['python3', '/work/upload_build.py'], check=True)
print('WIFI_ABI_REBUILD_COMPLETE', flush=True)
