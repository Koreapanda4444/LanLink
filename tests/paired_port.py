"""Select a loopback port usable by both real relay transports."""
import random
import socket
import sys


def allocate(exclude=()):
    tried=set()
    for _ in range(256):
        # A port selected by Windows TCP can be in an unrelated UDP exclusion
        # range. Choose independent candidates and require exclusive access.
        candidate=random.SystemRandom().randrange(10000,49152) if sys.platform=='win32' else 0
        if candidate and (candidate in tried or candidate in exclude):continue
        tried.add(candidate)
        with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as tcp, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
            if sys.platform=='win32':
                tcp.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
                udp.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
            try:
                tcp.bind(('127.0.0.1',candidate));tcp.listen(1)
                port=tcp.getsockname()[1]
                if port in exclude:continue
                udp.bind(('127.0.0.1',port))
                return port
            except OSError:continue
    raise RuntimeError('No loopback port available for both TCP and UDP after 256 independent probes')
