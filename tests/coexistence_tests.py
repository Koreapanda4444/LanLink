"""Verify both real Linux installers in both orders, including independent purge."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tarfile
import tempfile

assets=Path(sys.argv[1]).resolve()

def run(*args):subprocess.run(args,check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=40)
def snapshot(paths):return {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for root in paths for p in root.rglob('*') if p.is_file() and not p.is_symlink()}

with tempfile.TemporaryDirectory(prefix='lanlink-coexist-') as directory:
    workspace=Path(directory);packages={}
    for kind,pattern in [('client','LanLink-*-linux-x64.tar.gz'),('relay','LanLink-relay-*-linux-x86_64.tar.gz')]:
        archive=next(assets.glob(pattern));output=workspace/kind;output.mkdir()
        with tarfile.open(archive) as source:
            # Inputs have already passed release/prepare.py's package path validation.
            source.extractall(output,filter='data')
        marker=next(output.rglob('install-client.py' if kind=='client' else 'install-relay.sh'));packages[kind]=marker.parent
    certificate=workspace/'server.cert';key=workspace/'server.key';token=workspace/'master.token'
    run('openssl','req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1','-nodes','-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(certificate))
    token.write_text('private-server-master-token-for-coexistence-test-2026\n');token.chmod(0o600)
    for order in [('client','relay'),('relay','client')]:
        root=workspace/('-'.join(order));root.mkdir()
        client=packages['client'];relay=packages['relay']
        commands={
            'client':(sys.executable,str(client/'install-client.py'),'--root',str(root),'--no-start'),
            'relay':('bash',str(relay/'install-relay.sh'),'--root',str(root),'--certificate',str(certificate),'--private-key',str(key),'--token-file',str(token),'--no-start','--no-firewall')}
        paths={'client':[root/'opt/lanlink-client',root/'etc/lanlink-client',root/'var/lib/lanlink-client'], 'relay':[root/'opt/lanlink',root/'etc/lanlink',root/'var/lib/lanlink']}
        run(*commands[order[0]]);before=snapshot(paths[order[0]])
        assert not paths[order[1]][0].exists()
        run(*commands[order[1]]);assert snapshot(paths[order[0]])==before,'Installing the second component modified the first'
        # App-owned state and server-owned state are separate, including reinstall and purge.
        for kind in paths:
            state=paths[kind][-1]/'retained.state';state.write_bytes(kind.encode())
        client_before=snapshot(paths['client']);relay_before=snapshot(paths['relay'])
        run(*commands['client']);assert snapshot(paths['relay'])==relay_before
        run(*commands['relay']);assert snapshot(paths['client'])==client_before
        assert 'relay_require_device_registration=true' in (root/'etc/lanlink/relay.conf').read_text()
        relay_unit=(root/'etc/systemd/system/lanlink-relay.service').read_text()
        client_unit=(root/'etc/systemd/system/lanlink-client.service').read_text()
        assert 'User=lanlink-relay' in relay_unit and 'Group=lanlink-relay' in relay_unit
        assert '/var/lib/lanlink-client' in client_unit and 'Group=lanlink\n' in client_unit
        if order[0]=='client':
            run('bash',str(relay/'uninstall-relay.sh'),'--root',str(root),'--remove-data');assert snapshot(paths['client'])==client_before
            run(sys.executable,str(client/'uninstall-client.py'),'--root',str(root),'--purge')
        else:
            run(sys.executable,str(client/'uninstall-client.py'),'--root',str(root),'--purge');assert snapshot(paths['relay'])==relay_before
            run('bash',str(relay/'uninstall-relay.sh'),'--root',str(root),'--remove-data')
        assert all(not p.exists() for roots in paths.values() for p in roots)
print('Real Linux client/relay packages: client-only, relay-only, both installation orders, independent reinstall and purge passed')
