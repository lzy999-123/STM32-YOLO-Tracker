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
    def __init__(self, write_uart, clock=time.monotonic):
        self.write_uart = write_uart
        self.clock = clock
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
        self.status_seq = 0
        self.track_seq = 0
        self.track = None
        self.pending = None
        self.command_highwater = 0
        self.completed = collections.OrderedDict()
        self.outgoing = []
        self.retired = collections.deque(maxlen=64)

    def reply(self, kind, **fields):
        if self.peer is not None:
            message = dict(version=1, session=self.session, type=kind, **fields)
            self.outgoing.append((json.dumps(message, separators=(',', ':')).encode('ascii'), self.peer))

    def drain(self):
        packets, self.outgoing = self.outgoing, []
        return packets

    def release(self):
        if self.peer is not None:
            # 只封存已经结束的会话；活跃会话必须允许重试丢失的 welcome。
            self.retired.append((self.peer, self.session))
            # 停止/回中是幂等命令；随后停止 UART 心跳，保留 STM32 的链路超时保护。
            self.write_uart(frame(2, b'\x12') + frame(2, b'\x02'))
        self.peer = self.session = self.track = self.pending = None
        self.completed.clear()

    def packet(self, data, peer):
        if len(data) > 1200:
            return
        try:
            message = json.loads(data)
        except (ValueError, UnicodeError):
            return
        if not isinstance(message, dict) or type(message.get('version')) is not int or message['version'] != 1:
            return
        session = message.get('session')
        if not isinstance(session, str) or not 1 <= len(session) <= 64:
            return
        now = self.clock()
        if self.peer is not None and now - self.last_client > LEASE:
            self.release()
        kind = message.get('type')
        if kind == 'hello':
            if (peer, session) in self.retired:
                return
            if self.peer is None:
                self.peer, self.session = peer, session
                self.track_seq = self.command_highwater = self.status_seq = 0
                self.last_status = self.last_heartbeat = -100
            if peer == self.peer and session == self.session:
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
        if kind == 0x81 and len(payload) == 5:
            mode, horizontal, vertical = struct.unpack('>Bhh', payload)
            if mode not in (0, 1):
                return
            self.mode, self.horizontal, self.vertical = mode, horizontal / 10.0, vertical / 10.0
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
        if self.pending is not None and now - self.pending[2] >= .45:
            request, cmd, _ = self.pending
            # v2 UART 没有请求编号；不能重发非幂等步进/切换命令，否则可能重复执行。
            self.finish_command(request, cmd, 'command_failed')
            self.status_seq += 1
            self.reply('status', seq=self.status_seq, online=False, mode=self.mode,
                       horizontal=self.horizontal, vertical=self.vertical)
            self.release()
            return
        if now - self.last_heartbeat >= .2:
            self.write_uart(frame(3))
            self.last_heartbeat = now
        if self.track is not None and now - self.last_forward >= .02:
            x, y, received = self.track
            if now - received <= .2 and now - self.last_telemetry <= 1.0:
                self.write_uart(frame(1, struct.pack('>hh', x, y)))
                self.last_forward = now
            else:
                self.track = None
        if now - self.last_status >= .1:
            self.status_seq += 1
            self.reply('status', seq=self.status_seq, online=now - self.last_telemetry <= 1.0,
                       mode=self.mode, horizontal=self.horizontal, vertical=self.vertical)
            self.last_status = now


class SerialPort:
    def __init__(self, path, baud):
        import termios
        import fcntl
        if baud != 115200 or not path.startswith('/dev/tty'):
            raise ValueError('UART 必须明确配置为 /dev/tty*、115200')
        with open('/proc/cmdline', encoding='ascii') as source:
            if any(item.split(',')[0] == 'console=' + os.path.basename(path) for item in source.read().split()):
                raise ValueError('拒绝占用系统控制台 UART，请选择连接 STM32 的端口')
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

    def close(self):
        os.close(self.fd)


def main():
    def stop_service(_signal, _stack):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop_service)
    # Pico Plus 可供 Linux 使用的内存很少；避免仅为一个参数加载 argparse/logging。
    arguments = sys.argv[1:]
    if arguments and (len(arguments) != 2 or arguments[0] != '--config'):
        raise SystemExit('usage: luckfox-control.py [--config PATH]')
    config_path = arguments[1] if arguments else '/userdata/cfg/luckfox-control.json'
    with open(config_path, encoding='utf-8') as source:
        config = json.load(source)
    port = SerialPort(config['uart'], config.get('baud', 115200))
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    engine = Bridge(port.write)
    decoder = FrameParser()
    try:
        server.bind(('0.0.0.0', PORT))
        print('控制桥接已启动，UDP %s -> %s' % (PORT, config['uart']), file=sys.stderr, flush=True)
        while True:
            ready, _, _ = select.select([server, port.fd], [], [], .01)
            if server in ready:
                data, peer = server.recvfrom(1400)
                engine.packet(data, peer)
            if port.fd in ready:
                data = os.read(port.fd, 4096)
                for kind, payload in decoder.feed(data):
                    engine.uart_frame(kind, payload)
            engine.tick()
            for data, peer in engine.drain():
                server.sendto(data, peer)
    finally:
        try:
            engine.release()
        finally:
            port.close()
            server.close()


if __name__ == '__main__':
    main()
