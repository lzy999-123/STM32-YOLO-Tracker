import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('control_bridge', Path(__file__).resolve().parents[1] / 'board/luckfox/luckfox-control.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class ControlBridgeTests(unittest.TestCase):
    def setUp(self):
        self.now = 10.0
        self.writes = []
        self.engine = bridge.Bridge(self.writes.append, lambda: self.now)
        self.peer = ('127.0.0.1', 12345)
        self.send('hello')
        self.engine.drain()

    def send(self, kind, peer=None, **values):
        message = dict(type=kind, version=1, session='test-session', **values)
        self.engine.packet(json.dumps(message).encode(), peer or self.peer)

    def online(self):
        self.engine.uart_frame(0x81, struct.pack('>Bhh', 1, 900, 850))

    def test_duplicate_manual_step_executes_once_even_when_network_ack_is_lost(self):
        self.online()
        self.send('command', request=1, cmd=0x20)
        self.send('command', request=1, cmd=0x20)
        self.assertEqual(self.writes, [bridge.frame(2, b'\x20')])
        self.engine.uart_frame(0x82, b'\x20')
        original = self.engine.drain()
        self.send('command', request=1, cmd=0x20)
        self.assertEqual(original, self.engine.drain())
        self.assertEqual(len(self.writes), 1)

    def test_ack_timeout_never_repeats_uart_step(self):
        self.online()
        self.send('command', request=1, cmd=0x22)
        self.now += .5
        self.engine.tick()
        self.send('command', request=1, cmd=0x22)
        self.assertEqual(self.writes.count(bridge.frame(2, b'\x22')), 1)
        replies = [json.loads(data) for data, _ in self.engine.drain()]
        self.assertTrue(any(x['type'] == 'command_failed' for x in replies))

    def test_newest_coordinates_win_and_expired_coordinates_stop(self):
        self.online()
        self.send('track', seq=2, x=-100, y=50)
        self.send('track', seq=1, x=900, y=900)
        self.engine.tick()
        self.assertIn(bridge.frame(1, struct.pack('>hh', -100, 50)), self.writes)
        count = len(self.writes)
        self.now += .21
        self.engine.tick()
        self.assertFalse(any(bridge.FrameParser().feed(x)[0][0] == 1 for x in self.writes[count:]))

    def test_network_loss_stops_tracking_without_recenter_and_stops_uart_heartbeat(self):
        self.online()
        self.engine.tick()
        self.now += 1.01
        self.engine.tick()
        self.assertIsNone(self.engine.peer)
        # 只发送 0x12；不回中。手动模式角度保持，自动模式由 STM32 链路超时回中。
        self.assertEqual(self.writes[-1], bridge.frame(2, b'\x12'))
        self.assertNotIn(bridge.frame(2, b'\x02'), self.writes)
        count = len(self.writes)
        self.now += 2
        self.engine.tick()
        self.assertEqual(len(self.writes), count)

    def test_bye_sends_only_stop_tracking(self):
        self.online()
        self.send('bye')
        self.assertIsNone(self.engine.peer)
        self.assertEqual(self.writes, [bridge.frame(2, b'\x12')])

    def test_wrong_peer_and_invalid_coordinates_do_not_drive_uart(self):
        self.online()
        self.send('command', peer=('127.0.0.1', 9999), request=1, cmd=0x20)
        self.send('track', seq=1, x=True, y=5)
        self.send('track', seq=2, x=40000, y=5)
        self.assertIsNone(self.engine.track)
        self.assertEqual(self.writes, [])

    def test_socket_connection_without_telemetry_is_offline(self):
        self.engine.tick()
        status = [json.loads(data) for data, _ in self.engine.drain() if json.loads(data)['type'] == 'status']
        self.assertFalse(status[-1]['online'])
        self.send('command', request=1, cmd=0x11)
        self.assertNotIn(bridge.frame(2, b'\x11'), self.writes)

    def test_expired_session_cannot_be_reopened_by_delayed_hello(self):
        self.now += 1.01
        self.engine.tick()
        self.send('hello')
        self.assertIsNone(self.engine.peer)

    def test_lost_welcome_can_retry_hello_without_expiring_active_session(self):
        # setUp discarded the first welcome, as if that UDP reply was lost.
        self.now += .2
        self.send('hello')
        replies = [json.loads(data) for data, _ in self.engine.drain()]
        self.assertEqual([x['type'] for x in replies], ['welcome'])
        self.assertEqual(self.writes, [])
        self.now += .9
        self.engine.tick()
        self.assertEqual(self.engine.peer, self.peer)

    def test_center_clears_cached_offsets_before_uart_forwarding(self):
        self.online()
        self.send('track', seq=1, x=-100, y=50)
        self.engine.tick()
        self.send('command', request=2, cmd=0x02)
        self.engine.uart_frame(0x82, b'\x02')
        count = len(self.writes)
        self.now += .05
        self.engine.tick()
        self.assertIsNone(self.engine.track)
        self.assertFalse(any(kind == 1 for data in self.writes[count:]
                             for kind, _ in bridge.FrameParser().feed(data)))

    def test_uart_command_timeout_keeps_session_and_cannot_replay_step(self):
        self.online()
        self.send('command', request=1, cmd=0x22)
        self.now += .5
        self.engine.tick()
        replies = [json.loads(data) for data, _ in self.engine.drain()]
        self.assertIn({'version': 1, 'session': 'test-session', 'type': 'command_failed', 'request': 1, 'cmd': 0x22},
                      replies)
        self.assertEqual(self.engine.peer, self.peer)
        self.assertNotIn(bridge.frame(2, b'\x12'), self.writes)
        self.assertNotIn(bridge.frame(2, b'\x02'), self.writes)
        # 重发同一 request 只返回缓存结果；新 request 仍可执行。
        self.send('command', request=1, cmd=0x22)
        self.assertEqual(self.writes.count(bridge.frame(2, b'\x22')), 1)
        self.online()
        self.send('command', request=2, cmd=0x22)
        self.assertEqual(self.writes.count(bridge.frame(2, b'\x22')), 2)

    def test_same_track_seq_is_forwarded_once_and_latest_wins_within_slot(self):
        self.online()
        self.send('track', seq=1, x=10, y=20)
        self.engine.tick()
        self.now += .005
        self.send('track', seq=2, x=11, y=21)
        self.send('track', seq=3, x=12, y=22)
        self.engine.tick()  # 距上次转发不足 20ms，暂不发送
        for _ in range(5):
            self.now += .02
            self.engine.tick()
        tracks = [payload for data in self.writes for kind, payload in bridge.FrameParser().feed(data) if kind == 1]
        self.assertEqual(tracks, [struct.pack('>hh', 10, 20), struct.pack('>hh', 12, 22)])

    def test_six_byte_telemetry_reports_flags_and_five_byte_is_zero(self):
        self.engine.uart_frame(0x81, struct.pack('>BhhB', 1, -150, 2300, 0x05))
        self.assertEqual((self.engine.mode, self.engine.horizontal, self.engine.flags), (1, -15.0, 5))
        self.engine.tick()
        status = [json.loads(d) for d, _ in self.engine.drain() if json.loads(d)['type'] == 'status'][-1]
        self.assertEqual(status['flags'], 5)
        self.assertTrue(status['online'])
        self.engine.uart_frame(0x81, struct.pack('>Bhh', 0, 0, 0))
        self.assertEqual(self.engine.flags, 0)
        self.engine.uart_frame(0x81, struct.pack('>BhhBB', 1, 0, 0, 1, 1))
        self.assertEqual(self.engine.mode, 0)

    def test_fragmented_and_corrupted_uart_frames_resynchronize(self):
        valid = bridge.frame(0x81, struct.pack('>Bhh', 0, 900, 900))
        decoder = bridge.FrameParser()
        self.assertEqual(decoder.feed(b'garbage' + valid[:3]), [])
        self.assertEqual(decoder.feed(valid[3:]), [(0x81, struct.pack('>Bhh', 0, 900, 900))])
        corrupted = bytearray(valid)
        corrupted[-1] ^= 255
        self.assertEqual(decoder.feed(corrupted + valid), [(0x81, struct.pack('>Bhh', 0, 900, 900))])
        self.engine.uart_frame(0x81, struct.pack('>Bhh', 0, -150, 2300))
        self.assertEqual((self.engine.horizontal, self.engine.vertical), (-15.0, 230.0))


KEY = bytes.fromhex('00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff')


def signed(inner, key=KEY):
    return bridge.seal(json.dumps(inner), key)


class AuthTests(unittest.TestCase):
    def setUp(self):
        self.now = 10.0
        self.writes = []
        self.engine = bridge.Bridge(self.writes.append, lambda: self.now, auth_key=KEY)
        self.peer = ('127.0.0.1', 12345)

    def hello(self, session='s1', data=None):
        self.engine.packet(data or signed(dict(type='hello', version=1, session=session)), self.peer)
        return self.engine.drain()

    def test_signed_hello_accepted_and_reply_is_signed(self):
        replies = self.hello()
        self.assertEqual(self.engine.peer, self.peer)
        self.assertEqual(set(json.loads(replies[0][0])), {'m', 'auth'})
        self.assertEqual(bridge.unseal(replies[0][0], KEY)['type'], 'welcome')
        self.assertIsNone(bridge.unseal(replies[0][0], bytes(32)))

    def test_unsigned_and_bad_signature_rejected(self):
        self.hello(data=json.dumps(dict(type='hello', version=1, session='s1')).encode())
        self.assertIsNone(self.engine.peer)
        forged = json.loads(signed(dict(type='hello', version=1, session='s1')))
        forged['auth'] = forged['auth'][:-1] + ('0' if forged['auth'][-1] != '0' else '1')
        self.hello(data=json.dumps(forged).encode())
        self.assertIsNone(self.engine.peer)
        tampered = json.loads(signed(dict(type='hello', version=1, session='s1')))
        tampered['m'] = tampered['m'].replace('s1', 's2')
        self.hello(data=json.dumps(tampered).encode())
        self.assertIsNone(self.engine.peer)
        self.hello(data=signed(dict(type='hello', version=1, session='s1'), key=bytes(32)))
        self.assertIsNone(self.engine.peer)
        self.hello(data=b'{"m": "{}", "auth": "\xc3\xa9"}')
        self.assertIsNone(self.engine.peer)

    def test_replayed_hello_of_finished_session_rejected(self):
        captured = signed(dict(type='hello', version=1, session='s1'))
        self.hello(data=captured)
        self.engine.packet(signed(dict(type='bye', version=1, session='s1')), self.peer)
        self.assertIsNone(self.engine.peer)
        self.hello(data=captured)
        self.assertIsNone(self.engine.peer)
        self.hello(session='s2')
        self.assertEqual(self.engine.session, 's2')

    def test_key_parsing(self):
        self.assertIsNone(bridge.parse_key(''))
        self.assertIsNone(bridge.parse_key(None))
        self.assertEqual(bridge.parse_key('ab' * 16), bytes([0xab]) * 16)
        for bad in ('ab' * 15, 'zz' * 16, 'abc' * 11, 123):
            with self.assertRaises(ValueError):
                bridge.parse_key(bad)


class FailingPort:
    opened = 0

    def __init__(self, path, baud):
        FailingPort.opened += 1
        self.fd = 99
        self.fail_write = FailingPort.opened == 1
        self.data = []

    def write(self, data):
        if self.fail_write:
            raise OSError('EIO')
        self.data.append(data)

    def read(self):
        raise OSError('EIO')

    def close(self):
        pass


class UartRecoveryTests(unittest.TestCase):
    def setUp(self):
        FailingPort.opened = 0
        self.now = 0.0
        patcher = mock.patch.object(bridge, 'log')
        patcher.start()
        self.addCleanup(patcher.stop)
        self.link = bridge.UartLink('/dev/ttyS2', 115200, opener=FailingPort, clock=lambda: self.now)

    def test_write_error_closes_and_reopens_with_backoff(self):
        self.link.ensure_open()
        self.link.write(b'x')
        self.assertIsNone(self.link.port)
        self.link.write(b'dropped')  # 关闭期间丢弃，不抛异常
        self.now += .1
        self.link.ensure_open()
        self.assertIsNone(self.link.port)
        self.now += .5
        self.link.ensure_open()
        self.assertEqual(FailingPort.opened, 2)
        self.link.write(b'y')
        self.assertEqual(self.link.port.data, [b'y'])

    def test_read_error_closes_port(self):
        self.link.ensure_open()
        self.assertEqual(self.link.read(), [])
        self.assertIsNone(self.link.port)


class ServeLoopTests(unittest.TestCase):
    def test_sendto_and_uart_errors_do_not_stop_loop(self):
        calls = {'select': 0, 'sendto': 0}

        class Socket:
            def bind(self, address): pass
            def close(self): pass
            def recvfrom(self, size):
                return json.dumps(dict(type='hello', version=1, session='x')).encode(), ('127.0.0.1', 1)
            def sendto(self, data, peer):
                calls['sendto'] += 1
                raise OSError('ENETUNREACH')

        server = Socket()

        def fake_select(read, write, error, timeout):
            calls['select'] += 1
            if calls['select'] > 5:
                raise KeyboardInterrupt
            return list(read), [], []

        FailingPort.opened = 0
        with mock.patch.object(bridge.socket, 'socket', return_value=server), \
                mock.patch.object(bridge.select, 'select', fake_select), \
                mock.patch.object(bridge.UartLink.__init__, '__defaults__', (FailingPort, bridge.time.monotonic)), \
                mock.patch.object(bridge, 'log'):
            with self.assertRaises(KeyboardInterrupt):
                bridge.serve({'uart': '/dev/ttyS2'}, None)
        self.assertEqual(calls['select'], 6)
        self.assertGreaterEqual(calls['sendto'], 5)
        self.assertEqual(FailingPort.opened, 1)


spec_discovery = importlib.util.spec_from_file_location(
    'discovery', Path(__file__).resolve().parents[1] / 'board/luckfox/luckfox-discovery.py')
discovery = importlib.util.module_from_spec(spec_discovery)
spec_discovery.loader.exec_module(discovery)


class DiscoveryTests(unittest.TestCase):
    def test_signed_answer_and_missing_eth0(self):
        request = json.dumps(dict(type='luckfox-discover', version=1, nonce='n1')).encode()
        self.assertEqual(json.loads(discovery.answer(request, 'luckfox-1', None))['nonce'], 'n1')
        signed_answer = discovery.answer(request, 'luckfox-1', KEY)
        self.assertEqual(bridge.unseal(signed_answer, KEY)['device_id'], 'luckfox-1')
        self.assertIsNone(discovery.answer(b'\xff', 'luckfox-1', None))
        with tempfile.TemporaryDirectory() as root:
            for name, mac in (('lo', '00:00:00:00:00:00'), ('wlan0', 'AA:BB:CC:00:11:22')):
                Path(root, name).mkdir()
                Path(root, name, 'address').write_text(mac + '\n')
            self.assertEqual(discovery.device_id({}, root), 'luckfox-aabbcc001122')
            self.assertEqual(discovery.device_id({'device_id': 'luckfox-x'}, root), 'luckfox-x')


if __name__ == '__main__':
    unittest.main()
