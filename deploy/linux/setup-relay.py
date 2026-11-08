#!/usr/bin/env python3
"""Bootstrap a standalone relay, retaining private administrator material on the host."""
import argparse
import ipaddress
import os
from pathlib import Path
import re
import secrets
import subprocess
import tempfile

parser=argparse.ArgumentParser(description="Set up a private LanLink relay and issue a one-use device invitation")
parser.add_argument("host",help="Public hostname or IP used by clients")
parser.add_argument("--port",type=int,default=4433)
parser.add_argument("--root",default="/")
parser.add_argument("--no-start",action="store_true")
parser.add_argument("--no-firewall",action="store_true")
args=parser.parse_args()
if not 1<=args.port<=65535 or len(args.host)>253 or not re.fullmatch(r"[A-Za-z0-9.:-]+",args.host):parser.error("Invalid public host or port")
root=Path(args.root).resolve()
if root==Path("/") and os.geteuid()!=0:parser.error("Run with sudo")
package=Path(__file__).resolve().parent
os.umask(0o077)
config=root/"etc/lanlink/relay.conf"
if config.exists():parser.error("Relay already configured; use lanlink-relay --device-invite CONFIG HOST PORT instead")
with tempfile.TemporaryDirectory(prefix="lanlink-relay-setup-") as directory:
    work=Path(directory);cert=work/"server.cert";key=work/"server.key";token=work/"master.token"
    try:ipaddress.ip_address(args.host);san="IP:"+args.host
    except ValueError:san="DNS:"+args.host
    subprocess.run(["openssl","req","-x509","-newkey","ec","-pkeyopt","ec_paramgen_curve:prime256v1","-nodes","-days","365","-subj","/CN=LanLink Relay","-addext","subjectAltName="+san,"-addext","basicConstraints=critical,CA:FALSE","-addext","extendedKeyUsage=serverAuth","-keyout",str(key),"-out",str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    token.write_text(secrets.token_hex(32)+"\n")
    command=["bash",str(package/"install-relay.sh"),"--root",str(root),"--certificate",str(cert),"--private-key",str(key),"--token-file",str(token),"--port",str(args.port)]
    if args.no_start:command.append("--no-start")
    if args.no_firewall:command.append("--no-firewall")
    subprocess.run(command,check=True)
if root==Path("/") and not args.no_start:
    print("Share this short-lived invitation privately; administrator credentials remain on this server:",flush=True)
    subprocess.run(["runuser","-u","lanlink-relay","--",str(root/"opt/lanlink/bin/lanlink-relay"),"--device-invite",str(config),args.host,str(args.port)],check=True)
else:
    print("Offline installation complete. Issue a device invitation on the target host after starting the relay.")
