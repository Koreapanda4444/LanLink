"""Run against real relay, local IPC, client service and native tunnel on an isolated CI host."""
import hashlib
from windows_certificate_fixture import prepare_certificate,remove_certificate
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

relay, service, cli = (str(Path(p).resolve()) for p in sys.argv[1:4])
processes=[]
logs=[]
certificates=[]

def run(argv, success=True, env=None):
    result=subprocess.run(argv,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=25)
    assert (result.returncode==0)==success, result.stdout
    return result.stdout.strip()

def wait(fn, why, seconds=20):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        try:
            value=fn()
            if value:return value
        except (OSError,AssertionError,subprocess.SubprocessError):pass
        time.sleep(.1)
    raise AssertionError(why)

def spawn(argv,root,env=None):
    log=(root/("process-"+str(len(logs))+".log")).open("w+")
    logs.append(log);p=subprocess.Popen(argv,stdout=log,stderr=log,env=env);processes.append(p);return p

def stop(p,crash=False):
    if p.poll() is None:
        p.kill() if crash else p.terminate()
        p.wait(timeout=15)

def port():
    for _ in range(100):
        with socket.socket() as s:
            s.bind(("127.0.0.1",0));n=s.getsockname()[1]
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
                try:udp.bind(("127.0.0.1",n));return n
                except OSError:pass
    raise RuntimeError("Cannot allocate a relay port")

def ready(n):
    with socket.create_connection(("127.0.0.1",n),timeout=.5):return True

def field(text,name):
    return re.search(r"^"+name+r": (.*)$",text,re.M)[1]

def stale_request(endpoint,epoch,network):
    inner=b"LLCP"+bytes((1,4,0,0))+struct.pack("<I",16)+bytes.fromhex(network)
    body=struct.pack(">QI",epoch,len(inner))+inner
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(5);s.connect(str(endpoint));s.sendall(b"LLCP"+bytes((1,19,0,0))+struct.pack("<I",len(body))+body)
        header=b""
        while len(header)<12:header+=s.recv(12-len(header))
        assert header[6]==3, "Stale connection epoch was accepted on the new server"

with tempfile.TemporaryDirectory(prefix="lanlink-server-workflow-") as directory:
    root=Path(directory)
    if sys.platform!="win32":root.chmod(0o700)
    if sys.platform=="linux":run(["ip","link","set","lo","up"])
    try:
        servers=[]
        for name in ("a","b"):
            folder=root/name;folder.mkdir();n=port()
            run(["openssl","req","-x509","-newkey","ec","-pkeyopt","ec_paramgen_curve:prime256v1","-nodes","-days","1","-subj","/CN=LanLink Workflow","-addext","subjectAltName=IP:127.0.0.1","-addext","basicConstraints=critical,CA:FALSE","-addext","extendedKeyUsage=serverAuth","-keyout",str(folder/"key.pem"),"-out",str(folder/"cert.pem")])
            certificates.append(prepare_certificate(folder/"cert.pem",folder/"key.pem"))
            token=folder/"master.token";token.write_text("server-administrator-token-must-never-leave-server-"+name+"\n")
            if sys.platform!="win32":token.chmod(0o600)
            config=folder/"relay.conf"
            config.write_text(f"listen_host=127.0.0.1\nrelay_port={n}\ntls_certificate_file={folder/'cert.pem'}\ntls_private_key_file={folder/'key.pem'}\nauth_token_file={token}\nrelay_database_file={folder/'relay.db'}\nlog_directory={folder/'logs'}\nrelay_require_device_registration=true\n")
            server=spawn([relay,str(config)],root);wait(lambda:ready(n),"Relay did not start")
            pin=run([relay,"--certificate-pin",str(folder/"cert.pem")])
            servers.append((folder,n,config,pin,server))
        endpoint=root/"control.sock";env=dict(os.environ,LANLINK_CONTROL_SOCKET=str(endpoint))
        def control(*args,success=True):return run([cli,*args],success,env)
        def start_client(folder):
            folder.mkdir(exist_ok=True);config=folder/"client.conf"
            config.write_text(f"auth_token_file={folder/'unused.token'}\ndevice_identity_file={folder/'device.identity'}\nlog_directory={folder/'logs'}\nreconnect_initial_delay_ms=100\nreconnect_max_delay_ms=300\n")
            process=spawn([service,"--console",str(config)],root,env)
            wait(lambda:"epoch:" in control("server-list"),"Client IPC did not start")
            assert process.poll() is None
            return process
        def connected():return "authenticated: 1" in control("diagnostics")
        def epoch():return int(field(control("server-list"),"epoch"))
        owner_root=root/"owner";client=start_client(owner_root)
        assert "unconfigured" in control("status")
        ids=[]
        for index,(folder,n,config,pin,_) in enumerate(servers):
            ident=field(control("server-add","Server "+str(index),"127.0.0.1",str(n),pin),"result");ids.append(ident)
            assert not connected(), "Saving a server connected without user approval"
            code=run([relay,"--device-code",str(config)])
            control("server-enroll",ident,code,"owner "+str(index))
            control("server-enroll",ident,code,"owner "+str(index),success=False)
        a,b=ids;old=epoch()
        control("server-connect",a);wait(connected,"Individual device did not authenticate on server A")
        first=control("create","A private network");network=re.findall(r"\b[0-9a-f]{32}\b",first)[-1]
        wait(lambda:network in control("list"),"Created network missing")
        old=epoch()
        control("server-edit",a,"Renamed A","127.0.0.1",str(servers[0][1]),servers[0][3]);assert epoch()==old,"Name edit restarted the connection"
        control("server-connect",b);wait(connected,"Server B did not authenticate")
        assert network not in control("list"),"Server A network leaked into B"
        if sys.platform!="win32":stale_request(endpoint,old,network)
        control("server-connect",a);wait(connected,"Server A did not reconnect")
        wait(lambda:network in control("list"),"Server A membership was not restored")
        control("server-disconnect");wait(lambda:not connected(),"Disconnect kept the old session")
        assert field(control("server-list"),"selected")==""
        control("server-connect",a);wait(connected,"Reconnect failed")
        # Bad trust does not consume a registration code or damage other profiles.
        wrong=field(control("server-add","Wrong pin","127.0.0.1",str(servers[0][1]),"1"*64),"result")
        code=run([relay,"--device-code",str(servers[0][2])]);before=run([relay,"--device-list",str(servers[0][2])])
        control("server-enroll",wrong,code,"untrusted",success=False)
        assert run([relay,"--device-list",str(servers[0][2])])==before
        assert field(control("server-list"),"selected")==a
        control("server-delete",wrong)
        invitation_result=control("invite-link",network)
        network_link=field(invitation_result,"Invitation link")
        invitation=run([relay,"--device-invite",str(servers[0][2]),"127.0.0.1",str(servers[0][1]),network_link])
        control("stop");assert client.wait(timeout=15)==0
        invited_root=root/"invited";client=start_client(invited_root)
        control("server-accept",invitation,success=False)
        assert field(control("server-list"),"selected")=="", "Unconfirmed invitation changed the active server"
        control("server-accept",invitation,"--confirm-switch","--trust-pin")
        wait(connected,"Invited device was not registered")
        wait(lambda:network in control("list"),"Server invite did not continue to network joining")
        credentials=list((invited_root/"profiles").rglob("*.credential"));assert len(credentials)==1
        digest=hashlib.sha256(credentials[0].read_bytes()).digest()
        # Simulate a service crash; saved individual credentials and selection survive.
        stop(client,crash=True)
        if sys.platform!="win32":endpoint.unlink(missing_ok=True)
        client=start_client(invited_root);wait(connected,"Saved selection did not reconnect after a crash")
        assert hashlib.sha256(credentials[0].read_bytes()).digest()==digest
        # Changing the actual server key blocks automatic reconnect until the trusted key returns.
        folder,n,config,pin,server=servers[0]
        cert=(folder/"cert.pem").read_bytes();key=(folder/"key.pem").read_bytes();stop(server)
        run(["openssl","req","-x509","-newkey","ec","-pkeyopt","ec_paramgen_curve:prime256v1","-nodes","-days","1","-subj","/CN=Changed Relay","-addext","subjectAltName=IP:127.0.0.1","-keyout",str(folder/"key.pem"),"-out",str(folder/"cert.pem")])
        certificates.append(prepare_certificate(folder/"cert.pem",folder/"key.pem"))
        changed=spawn([relay,str(config)],root);wait(lambda:ready(n),"Changed-key relay did not restart")
        wait(lambda:not connected() and "changed" in control("diagnostics"),"Changed server key was not blocked")
        stop(changed);(folder/"cert.pem").write_bytes(cert);(folder/"key.pem").write_bytes(key)
        restored=spawn([relay,str(config)],root);wait(lambda:ready(n),"Restored relay did not start");wait(connected,"Trusted server reconnect failed")
        devices=run([relay,"--device-list",str(config)])
        invited_id=next(line.split()[0] for line in devices.splitlines() if "Invited device" in line)
        run([relay,"--device-revoke",str(config),invited_id]);wait(lambda:not connected(),"Live device revocation did not close the session")
        if sys.platform!="win32":
            assert all(p.stat().st_mode&0o077==0 for p in (invited_root/"profiles").rglob("*") if p.is_file()),"Profile secrets are readable by other users"
        control("stop");assert client.wait(timeout=15)==0
        print("Real service onboarding: isolated profiles, one-use registration, switch/disconnect, stale request rejection, confirmed server/network invitation, crash recovery, changed-key blocking and live revocation passed")
    except BaseException:
        for log in logs:
            log.flush();log.seek(0);print(log.read()[-5000:])
        raise
    finally:
        for process in reversed(processes):stop(process)
        for log in logs:log.close()
        for thumb in reversed(certificates):remove_certificate(thumb)
