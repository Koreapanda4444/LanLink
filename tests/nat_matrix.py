"""Disposable kernel NAT test; run as root inside a fresh `unshare --net`."""
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import tempfile
import time
import uuid


def run(*args, **options):
    return subprocess.run([str(a) for a in args], check=True, timeout=30, **options)


def main():
    binary, relay, report = (Path(p).resolve() for p in sys.argv[1:])
    devices = json.loads(subprocess.check_output(['ip', '-json', 'link', 'show']))
    if os.geteuid() != 0 or {device['ifname'] for device in devices} != {'lo'}:
        raise RuntimeError('run in a new, disposable root network namespace with only lo')
    os.umask(0o077)
    prefix = 'll59-' + uuid.uuid4().hex[:8]
    namespaces = {name: prefix + '-' + name for name in ('a', 'ar', 'b', 'br')}
    def ip(*args):
        run('ip', *args, stdout=subprocess.DEVNULL)
    def inside(name, *args):
        return ['ip', 'netns', 'exec', namespaces[name], *map(str, args)]
    results = []
    try:
        ip('link', 'set', 'lo', 'up')
        ip('link', 'add', 'llwan0', 'type', 'bridge')
        ip('addr', 'add', '203.0.113.254/24', 'dev', 'llwan0')
        ip('link', 'set', 'llwan0', 'up')
        for name in namespaces.values():
            ip('netns', 'add', name)
            ip('-n', name, 'link', 'set', 'lo', 'up')
        for index, name in enumerate(('a', 'b'), 1):
            router = name + 'r'
            ip('link', 'add', name+'lan', 'type', 'veth', 'peer', 'name', router+'lan')
            ip('link', 'set', name+'lan', 'netns', namespaces[name])
            ip('link', 'set', router+'lan', 'netns', namespaces[router])
            for node, device, address in ((name,name+'lan',f'10.{10+index}.0.2/24'), (router,router+'lan',f'10.{10+index}.0.1/24')):
                ip('-n', namespaces[node], 'addr', 'add', address, 'dev', device)
                ip('-n', namespaces[node], 'link', 'set', device, 'up')
            ip('-n', namespaces[name], 'route', 'add', 'default', 'via', f'10.{10+index}.0.1')
            ip('link', 'add', name+'wan', 'type', 'veth', 'peer', 'name', router+'wan')
            ip('link', 'set', router+'wan', 'netns', namespaces[router])
            ip('link', 'set', name+'wan', 'master', 'llwan0')
            ip('link', 'set', name+'wan', 'up')
            ip('-n', namespaces[router], 'addr', 'add', f'203.0.113.{index}/24', 'dev', router+'wan')
            ip('-n', namespaces[router], 'link', 'set', router+'wan', 'up')
            run(*inside(router, 'sysctl', '-q', '-w', 'net.ipv4.ip_forward=1'))
            run(*inside(router, 'iptables', '-t', 'nat', '-A', 'POSTROUTING', '-p', 'udp', '--sport', 50000+index,
                        '-j', 'SNAT', '--to-source', f'203.0.113.{index}:{40000+index}'))
            run(*inside(router, 'iptables', '-t', 'nat', '-A', 'POSTROUTING', '-o', router+'wan', '-j', 'MASQUERADE'))
            run(*inside(router, 'iptables', '-t', 'nat', '-A', 'PREROUTING', '-p', 'udp', '--dport', 40000+index,
                        '-j', 'DNAT', '--to-destination', f'10.{10+index}.0.2:{50000+index}'))
        with tempfile.TemporaryDirectory(prefix='lanlink-nat-') as temporary:
            root = Path(temporary)
            run(binary, root, 'prepare')
            run('openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-days', '1',
                '-subj', '/CN=203.0.113.254', '-addext', 'subjectAltName=IP:203.0.113.254', '-keyout', root/'key.pem', '-out', root/'cert.pem',
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            (root/'auth.token').write_text('lanlink-kernel-nat-disposable-test-token-2026\n')
            (root/'relay.conf').write_text(f'listen_host=0.0.0.0\nrelay_port=4433\ntls_certificate_file={root}/cert.pem\ntls_private_key_file={root}/key.pem\nauth_token_file={root}/auth.token\nrelay_database_file={root}/relay.db\nlog_directory={root}/logs\n')
            environment = dict(os.environ, SSL_CERT_FILE=str(root/'cert.pem'))
            with (root/'relay-output.log').open('w+') as output:
                server = subprocess.Popen([str(relay), str(root/'relay.conf')], stdout=output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 10
                    while True:
                        try:
                            with socket.create_connection(('203.0.113.254',4433), timeout=0.2):
                                break
                        except OSError:
                            if server.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError('NAT fixture Relay did not start')
                            time.sleep(0.05)
                    for mode in ('direct', 'blocked'):
                        if mode == 'blocked':
                            for router in ('ar','br'):
                                run(*inside(router, 'iptables', '-I', 'FORWARD', '1', '-p', 'udp', '-j', 'DROP'))
                        processes = []
                        try:
                            for index, name in enumerate(('a','b'), 1):
                                processes.append(subprocess.Popen(inside(name,binary,root,name,40003-index,50000+index,mode),
                                                                  env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
                            messages = []
                            for process in processes:
                                text, _ = process.communicate(timeout=40)
                                if process.returncode:
                                    raise RuntimeError(text)
                                messages.append(text.strip())
                            results.append({'scenario':mode,'status':'passed','evidence':messages})
                            print(json.dumps(results[-1]), flush=True)
                        finally:
                            for process in processes:
                                if process.poll() is None:
                                    process.kill(); process.wait(timeout=5)
                except Exception:
                    output.flush(); print((root/'relay-output.log').read_text()[-4000:])
                    raise
                finally:
                    server.terminate()
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill(); server.wait(timeout=5)
        report.write_text(json.dumps({'source_commit':os.getenv('GITHUB_SHA','local'),'kernel':platform.release(),
                                      'environment':'Linux namespaces with two SNAT/DNAT routers; known mapped candidates',
                                      'results':results}, indent=2)+'\n')
        report.chmod(0o644)
    finally:
        for name in reversed(list(namespaces.values())):
            subprocess.run(['ip','netns','del',name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)


if __name__ == '__main__':
    main()
