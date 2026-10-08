#!/usr/bin/python3
"""Reply to camera discovery on every IPv4 interface without exposing Wi-Fi secrets."""
import json
import os
import signal
import socket
import sys
import time

PORT = 39093
DEFAULT_CONFIG = '/userdata/cfg/luckfox-control.json'
_last_log = {}


def log(message, key=None, interval=30.0):
    if key is not None:
        now = time.monotonic()
        if key in _last_log and now - _last_log[key] < interval:
            return
        _last_log[key] = now
    print(time.strftime('%Y-%m-%d %H:%M:%S ') + message, file=sys.stderr, flush=True)


def parse_key(text):
    """与 luckfox-control.py 相同：空表示不签名，否则至少 32 个十六进制字符。"""
    if text is None or text == '':
        return None
    if not isinstance(text, str) or len(text) < 32 or len(text) % 2:
        raise ValueError('auth_key 必须是至少 32 个字符的十六进制字符串')
    try:
        return bytes.fromhex(text)
    except ValueError:
        raise ValueError('auth_key 必须是至少 32 个字符的十六进制字符串') from None


def seal(inner, key):
    if key is None:
        return inner.encode('utf-8')
    import hashlib
    import hmac
    auth = hmac.new(key, inner.encode('utf-8'), hashlib.sha256).hexdigest()
    return json.dumps({'m': inner, 'auth': auth}, separators=(',', ':')).encode('ascii')


def load_config(path):
    """发现服务在没有控制配置时也要运行，此时不签名、监听全部地址。"""
    try:
        with open(path, encoding='utf-8') as source:
            config = json.load(source)
    except FileNotFoundError:
        return {}, None
    if not isinstance(config, dict):
        raise ValueError('配置必须是 JSON 对象')
    return config, parse_key(config.get('auth_key'))


def device_id(config, root='/sys/class/net'):
    configured = config.get('device_id')
    if configured:
        if not isinstance(configured, str) or not configured.startswith('luckfox-') or len(configured) > 64:
            raise ValueError('device_id 必须以 luckfox- 开头且不超过 64 字符')
        return configured
    # 优先 eth0 以保持既有 ID；缺失时退回 wlan0 或任意有效网卡 MAC。
    try:
        others = sorted(os.listdir(root))
    except OSError:
        others = []
    for name in ['eth0', 'wlan0'] + others:
        if name == 'lo':
            continue
        try:
            with open(os.path.join(root, name, 'address'), encoding='ascii') as source:
                mac = source.read().strip().replace(':', '').lower()
        except (OSError, ValueError):
            continue
        if len(mac) == 12 and mac != '000000000000' and all(c in '0123456789abcdef' for c in mac):
            return 'luckfox-' + mac
    raise OSError('没有可用于设备标识的网卡 MAC')


def answer(packet, ident, key):
    """返回回应字节；非发现请求返回 None。"""
    try:
        request = json.loads(packet)
    except (ValueError, UnicodeError):
        return None
    if not isinstance(request, dict):
        return None
    if request.get('type') != 'luckfox-discover' or request.get('version') != 1:
        return None
    nonce = request.get('nonce', '')
    if not isinstance(nonce, str) or not 1 <= len(nonce) <= 64:
        return None
    response = {'type': 'luckfox-camera', 'version': 1, 'nonce': nonce,
                'device_id': ident, 'rtsp_port': 554, 'path': '/live/0'}
    return seal(json.dumps(response, separators=(',', ':')), key)


def serve(config, key):
    ident = device_id(config)
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # 绑定具体地址时 Linux 不再收到广播，Qt 会退回本网段单播查找。
        server.bind((config.get('bind_address', '0.0.0.0'), PORT))
        log('发现服务已启动，device_id=%s，签名=%s' % (ident, '是' if key else '否'))
        while True:
            try:
                packet, peer = server.recvfrom(512)
            except OSError as error:
                log('UDP 接收失败：%s' % error, key='recv')
                time.sleep(.1)
                continue
            response = answer(packet, ident, key)
            if response is None:
                continue
            try:
                server.sendto(response, peer)
            except OSError as error:
                log('UDP 发送失败：%s' % error, key='send')
    finally:
        server.close()


def main():
    def stop_service(_signal, _stack):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop_service)
    arguments = sys.argv[1:]
    if arguments and (len(arguments) != 2 or arguments[0] != '--config'):
        raise SystemExit('usage: luckfox-discovery.py [--config PATH]')
    try:
        config, key = load_config(arguments[1] if arguments else DEFAULT_CONFIG)
    except (OSError, ValueError) as error:
        log('配置无效，不启动：%s' % error)
        raise SystemExit(2)
    if key is None:
        log('警告：未配置 auth_key，发现回应不签名')
    delay = 1.0
    while True:
        started = time.monotonic()
        try:
            serve(config, key)
        except (SystemExit, KeyboardInterrupt):
            raise
        except Exception:
            import traceback
            log('发现服务异常，%.0fs 后重启：\n%s' % (delay, traceback.format_exc()))
        if time.monotonic() - started > 60:
            delay = 1.0
        time.sleep(delay)
        delay = min(delay * 2, 30.0)


if __name__ == '__main__':
    main()
