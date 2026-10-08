#!/usr/bin/python3
"""Wi-Fi control bridge. UART is used only here, never on the Qt computer."""
import collections
import json
import os
import select
import signal
import socket
import struct
import sys
import time

COMMANDS = {0x01, 0x02, 0x11, 0x12, 0x20, 0x21, 0x22, 0x23}
PORT = 5005
LEASE = 1.0
ACK_TIMEOUT = .45
FORWARD_INTERVAL = .02
TRACK_MAX_AGE = .2
SEEN_SESSIONS = 256
DEFAULT_CONFIG = '/userdata/cfg/luckfox-control.json'
_last_log = {}


def log(message, key=None, interval=30.0):
    """写 stderr；同类错误按 key 限速，避免持续故障写满内存盘 /tmp。"""
    if key is not None:
        now = time.monotonic()
        if key in _last_log and now - _last_log[key] < interval:
            return
        _last_log[key] = now
    print(time.strftime('%Y-%m-%d %H:%M:%S ') + message, file=sys.stderr, flush=True)


def parse_key(text):
    """auth_key 为空表示不认证；否则必须是至少 32 个十六进制字符。"""
    if text is None or text == '':
        return None
    if not isinstance(text, str) or len(text) < 32 or len(text) % 2:
        raise ValueError('auth_key 必须是至少 32 个字符的十六进制字符串')
    try:
        return bytes.fromhex(text)
    except ValueError:
        raise ValueError('auth_key 必须是至少 32 个字符的十六进制字符串') from None


def _mac(key, inner):
    import hashlib
    import hmac
    return hmac.new(key, inner.encode('utf-8'), hashlib.sha256).hexdigest()


def seal(inner, key):
    """inner 为内层 JSON 文本；配置密钥时包装为 {"m": 文本, "auth": HMAC-SHA256}。"""
    if key is None:
        return inner.encode('utf-8')
    return json.dumps({'m': inner, 'auth': _mac(key, inner)}, separators=(',', ':')).encode('ascii')


def unseal(data, key):
    """返回内层 JSON 对象；格式错误或签名无效返回 None。"""
    try:
        message = json.loads(data)
        if key is not None:
            if not isinstance(message, dict) or set(message) != {'m', 'auth'}:
                return None
            inner, auth = message['m'], message['auth']
            if not isinstance(inner, str) or not isinstance(auth, str) or len(auth) != 64:
                return None
            import hmac
            if not hmac.compare_digest(_mac(key, inner).encode('ascii'), auth.encode('utf-8')):
                return None
            message = json.loads(inner)
    except (ValueError, UnicodeError):
        return None
    return message if isinstance(message, dict) else None


def crc8(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value << 1) ^ (7 if value & 128 else 0)) & 255
    return value


def frame(kind, payload=b''):
    body = bytes((len(payload) + 1, kind)) + payload
    return b'\xa5\x5a' + body + bytes((crc8(body),))


class FrameParser:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while len(self.buffer) >= 5:
            length = self.buffer[2]
            if self.buffer[:2] != b'\xa5\x5a' or not 1 <= length <= 32:
                del self.buffer[0]
                continue
            size = length + 4
            if len(self.buffer) < size:
                break
            if crc8(self.buffer[2:size - 1]) != self.buffer[size - 1]:
                del self.buffer[0]
                continue
            result.append((self.buffer[3], bytes(self.buffer[4:size - 1])))
            del self.buffer[:size]
        if len(self.buffer) > 4096:
            self.buffer.clear()
        return result


def integer(value, low, high):
    return type(value) is int and low <= value <= high


class Bridge:
    """Protocol engine; the clock and UART writer can be injected for verification."""
    def __init__(self, write_uart, clock=time.monotonic, auth_key=None):
        self.write_uart = write_uart
        self.clock = clock
        self.auth_key = auth_key
        self.peer = None
        self.session = None
        self.last_client = 0
        self.last_telemetry = -100
        self.last_heartbeat = -100
        self.last_status = -100
        self.last_forward = -100
        self.mode = 0
        self.horizontal = 0.0
        self.vertical = 0.0
        self.flags = 0
        self.status_seq = 0
        self.track_seq = 0
        self.track = None
        self.pending = None
        self.command_highwater = 0
        self.completed = collections.OrderedDict()
        self.outgoing = []
        # 用过的 session 不允许再次 hello，防止重放旧会话（有界，板端重启后清空）。
        self.seen_sessions = collections.OrderedDict()

    def reply(self, kind, **fields):
        if self.peer is not None:
            message = dict(version=1, session=self.session, type=kind, **fields)
            self.outgoing.append((seal(json.dumps(message, separators=(',', ':')), self.auth_key), self.peer))

    def drain(self):
        packets, self.outgoing = self.outgoing, []
        return packets

    def release(self):
        if self.peer is not None:
            # 只停止跟踪，不回中；随后停止 UART 心跳。STM32 的 1s 链路超时只在自动模式回中，手动模式保持角度。
            self.write_uart(frame(2, b'\x12'))
        self.peer = self.session = self.track = self.pending = None
        self.completed.clear()

    def packet(self, data, peer):
        if len(data) > 1200:
            return
        message = unseal(data, self.auth_key)
        if message is None or type(message.get('version')) is not int or message['version'] != 1:
            return
        session = message.get('session')
        if not isinstance(session, str) or not 1 <= len(session) <= 64:
            return
        now = self.clock()
        if self.peer is not None and now - self.last_client > LEASE:
            self.release()
        kind = message.get('type')
        if kind == 'hello':
            if self.peer is None:
                if session in self.seen_sessions:
                    return
                self.seen_sessions[session] = None
                while len(self.seen_sessions) > SEEN_SESSIONS:
                    self.seen_sessions.popitem(last=False)
                self.peer, self.session = peer, session
                self.track_seq = self.command_highwater = self.status_seq = 0
                self.last_status = self.last_heartbeat = -100
            if peer == self.peer and session == self.session:
                # 活跃会话允许重复 hello，以便重试丢失的 welcome。
                self.last_client = now
                self.reply('welcome')
            return
        if peer != self.peer or session != self.session:
            return
        if kind == 'bye':
            self.release()
        elif kind == 'heartbeat':
            self.last_client = now
        elif kind == 'track':
            seq, x, y = message.get('seq'), message.get('x'), message.get('y')
            if not integer(seq, 1, 0xffffffff) or seq <= self.track_seq:
                return
            if not integer(x, -32768, 32767) or not integer(y, -32768, 32767):
                return
            self.last_client, self.track_seq = now, seq
            self.track = (x, y, now)
        elif kind == 'command':
            request, cmd = message.get('request'), message.get('cmd')
            if not integer(request, 1, 0xffffffff) or not integer(cmd, 0, 255) or cmd not in COMMANDS:
                return
            self.last_client = now
            if request in self.completed:
                old_cmd, result = self.completed[request]
                if old_cmd == cmd:
                    self.reply(result, request=request, cmd=cmd)
                return
            if self.pending is not None or request <= self.command_highwater:
                return
            self.command_highwater = request
            if now - self.last_telemetry > 1.0:
                self.finish_command(request, cmd, 'command_failed')
                return
            if cmd in (0x01, 0x02, 0x11, 0x12):
                self.track = None
            self.pending = (request, cmd, now)
            self.write_uart(frame(2, bytes((cmd,))))

    def finish_command(self, request, cmd, result):
        self.completed[request] = (cmd, result)
        while len(self.completed) > 64:
            self.completed.popitem(last=False)
        self.reply(result, request=request, cmd=cmd)
        self.pending = None

    def uart_frame(self, kind, payload):
        if kind == 0x81 and len(payload) in (5, 6):
            # 旧固件 5 字节；新固件追加 uint8 flags（bit0 故障、bit1 急停、bit2 链路丢失）。
            mode, horizontal, vertical = struct.unpack('>Bhh', payload[:5])
            if mode not in (0, 1):
                return
            self.mode, self.horizontal, self.vertical = mode, horizontal / 10.0, vertical / 10.0
            self.flags = payload[5] if len(payload) == 6 else 0
            self.last_telemetry = self.clock()
        elif kind == 0x82 and len(payload) == 1 and self.pending is not None:
            request, cmd, _ = self.pending
            if payload[0] == cmd:
                self.finish_command(request, cmd, 'ack')

    def tick(self):
        if self.peer is None:
            return
        now = self.clock()
        if now - self.last_client > LEASE:
            self.release()
            return
        if self.pending is not None and now - self.pending[2] >= ACK_TIMEOUT:
            request, cmd, _ = self.pending
            # v2 UART 没有请求编号；不能重发非幂等步进/切换命令。只报告失败，保留会话。
            self.finish_command(request, cmd, 'command_failed')
        if now - self.last_heartbeat >= .2:
            self.write_uart(frame(3))
            self.last_heartbeat = now
        if self.track is not None:
            x, y, received = self.track
            if now - received > TRACK_MAX_AGE or now - self.last_telemetry > 1.0:
                self.track = None
            elif now - self.last_forward >= FORWARD_INTERVAL:
                # 每个新序号只转发一次；更快到达的新序号在下一个 20ms 时隙转发最新值。
                self.write_uart(frame(1, struct.pack('>hh', x, y)))
                self.last_forward = now
                self.track = None
        if now - self.last_status >= .1:
            self.status_seq += 1
            self.reply('status', seq=self.status_seq, online=now - self.last_telemetry <= 1.0,
                       mode=self.mode, horizontal=self.horizontal, vertical=self.vertical, flags=self.flags)
            self.last_status = now


def check_uart_config(path, baud):
    if baud != 115200 or not isinstance(path, str) or not path.startswith('/dev/tty'):
        raise ValueError('UART 必须明确配置为 /dev/tty*、115200')
    with open('/proc/cmdline', encoding='ascii') as source:
        if any(item.split(',')[0] == 'console=' + os.path.basename(path) for item in source.read().split()):
            raise ValueError('拒绝占用系统控制台 UART，请选择连接 STM32 的端口')


class SerialPort:
    def __init__(self, path, baud):
        import termios
        import fcntl
        check_uart_config(path, baud)
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
            settings = termios.tcgetattr(self.fd)
            settings[0] = settings[1] = settings[3] = 0
            settings[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            settings[4] = settings[5] = termios.B115200
            settings[6][termios.VMIN] = 0
            settings[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, settings)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        except Exception:
            os.close(self.fd)
            raise

    def write(self, data):
        deadline = time.monotonic() + .05
        while data:
            _, ready, _ = select.select([], [self.fd], [], max(0, deadline - time.monotonic()))
            if not ready:
                raise OSError('UART 写入超时')
            try:
                count = os.write(self.fd, data)
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise OSError('UART 写入超时')
                continue
            if count <= 0:
                raise OSError('UART 未写入数据')
            data = data[count:]

    def read(self):
        return os.read(self.fd, 4096)

    def close(self):
        os.close(self.fd)


class UartLink:
    """串口读写出错时记录日志、关闭并按退避重开；异常不会终止主循环。"""
    def __init__(self, path, baud, opener=SerialPort, clock=time.monotonic):
        self.path, self.baud, self.opener, self.clock = path, baud, opener, clock
        self.port = None
        self.retry_at = 0.0
        self.backoff = .5
        self.parser = FrameParser()

    def fd(self):
        return None if self.port is None else self.port.fd

    def ensure_open(self):
        if self.port is not None or self.clock() < self.retry_at:
            return
        try:
            self.port = self.opener(self.path, self.baud)
        except OSError as error:
            self.fail('打开', error)
            return
        self.parser = FrameParser()
        self.backoff = .5
        log('UART 已打开 %s' % self.path)

    def fail(self, action, error):
        log('UART %s失败：%s；%.1fs 后重开' % (action, error, self.backoff), key='uart-' + action)
        self.close()
        self.retry_at = self.clock() + self.backoff
        self.backoff = min(self.backoff * 2, 5.0)

    def write(self, data):
        # 串口恢复前直接丢弃；等待中的命令会按 ACK 超时报告失败。
        if self.port is None:
            return
        try:
            self.port.write(data)
        except OSError as error:
            self.fail('写入', error)

    def read(self):
        if self.port is None:
            return []
        try:
            data = self.port.read()
        except BlockingIOError:
            return []
        except OSError as error:
            self.fail('读取', error)
            return []
        if not data:
            # select 报告可读却读到 EOF，说明设备已挂断；不处理会空转占满 CPU。
            self.fail('读取', 'EOF')
            return []
        return self.parser.feed(data)

    def close(self):
        if self.port is not None:
            port, self.port = self.port, None
            try:
                port.close()
            except OSError:
                pass


def serve(config, auth_key):
    uart = UartLink(config['uart'], config.get('baud', 115200))
    engine = Bridge(uart.write, auth_key=auth_key)
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        address = config.get('bind_address', '0.0.0.0')
        server.bind((address, PORT))
        log('控制桥接已启动，UDP %s:%s -> %s' % (address, PORT, config['uart']))
        while True:
            uart.ensure_open()
            uart_fd = uart.fd()
            sources = [server] if uart_fd is None else [server, uart_fd]
            ready, _, _ = select.select(sources, [], [], .01)
            if server in ready:
                try:
                    data, peer = server.recvfrom(1400)
                except OSError as error:
                    log('UDP 接收失败：%s' % error, key='recv')
                else:
                    engine.packet(data, peer)
            if uart_fd is not None and uart_fd in ready:
                for kind, payload in uart.read():
                    engine.uart_frame(kind, payload)
            engine.tick()
            for data, peer in engine.drain():
                try:
                    server.sendto(data, peer)
                except OSError as error:
                    log('UDP 发送失败：%s' % error, key='send')
    finally:
        try:
            engine.release()
        finally:
            uart.close()
            server.close()


def load_config(path):
    with open(path, encoding='utf-8') as source:
        config = json.load(source)
    if not isinstance(config, dict):
        raise ValueError('配置必须是 JSON 对象')
    check_uart_config(config['uart'], config.get('baud', 115200))
    if not isinstance(config.get('bind_address', '0.0.0.0'), str):
        raise ValueError('bind_address 必须是 IPv4 地址字符串')
    return config, parse_key(config.get('auth_key'))


def main():
    def stop_service(_signal, _stack):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop_service)
    # Pico Plus 可供 Linux 使用的内存很少；避免仅为一个参数加载 argparse/logging。
    arguments = sys.argv[1:]
    if arguments and (len(arguments) != 2 or arguments[0] != '--config'):
        raise SystemExit('usage: luckfox-control.py [--config PATH]')
    try:
        config, auth_key = load_config(arguments[1] if arguments else DEFAULT_CONFIG)
    except (OSError, ValueError, KeyError) as error:
        log('配置无效，不启动：%s' % error)
        raise SystemExit(2)  # 启动脚本遇到 2 不再重启
    if auth_key is None:
        log('警告：未配置 auth_key，接受未认证的旧格式消息，同一网络内任何设备都能控制云台')
    delay = 1.0
    while True:
        started = time.monotonic()
        try:
            serve(config, auth_key)
        except (SystemExit, KeyboardInterrupt):
            raise
        except Exception:
            import traceback
            log('控制桥接异常，%.0fs 后重启：\n%s' % (delay, traceback.format_exc()))
        if time.monotonic() - started > 60:
            delay = 1.0
        time.sleep(delay)
        delay = min(delay * 2, 30.0)


if __name__ == '__main__':
    main()
