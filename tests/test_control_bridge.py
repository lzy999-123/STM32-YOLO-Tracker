import importlib.util
import json
from pathlib import Path
import struct
import unittest

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

    def test_network_loss_stops_gimbal_and_uart_heartbeat(self):
        self.online()
        self.engine.tick()
        self.now += 1.01
        self.engine.tick()
        self.assertIsNone(self.engine.peer)
        self.assertEqual(self.writes[-1], bridge.frame(2, b'\x12') + bridge.frame(2, b'\x02'))
        count = len(self.writes)
        self.now += 2
        self.engine.tick()
        self.assertEqual(len(self.writes), count)

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

    def test_uart_command_timeout_releases_owner_and_cannot_replay_step(self):
        self.online()
        self.send('command', request=1, cmd=0x22)
        self.now += .5
        self.engine.tick()
        self.send('hello')
        self.send('command', request=1, cmd=0x22)
        self.assertIsNone(self.engine.peer)
        self.assertEqual(self.writes.count(bridge.frame(2, b'\x22')), 1)

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


if __name__ == '__main__':
    unittest.main()
