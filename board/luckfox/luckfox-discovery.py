#!/usr/bin/python3
"""Reply to camera discovery on every IPv4 interface without exposing Wi-Fi secrets."""
import json
import socket

PORT = 39093
with open('/sys/class/net/eth0/address', encoding='ascii') as source:
    device_id = 'luckfox-' + source.read().strip().replace(':', '')
server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server.bind(('0.0.0.0', PORT))
while True:
    packet, peer = server.recvfrom(512)
    try:
        request = json.loads(packet)
        if not isinstance(request, dict):
            continue
        if request.get('type') != 'luckfox-discover' or request.get('version') != 1:
            continue
        nonce = request.get('nonce', '')
        if not isinstance(nonce, str) or not 1 <= len(nonce) <= 64:
            continue
        response = {'type': 'luckfox-camera', 'version': 1, 'nonce': nonce,
                    'device_id': device_id, 'rtsp_port': 554, 'path': '/live/0'}
        server.sendto(json.dumps(response).encode('ascii'), peer)
    except (ValueError, TypeError, OSError):
        continue
