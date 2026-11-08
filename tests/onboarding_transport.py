import os
from windows_certificate_fixture import prepare_certificate,remove_certificate
from pathlib import Path
from paired_port import allocate
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="lanlink-onboarding-") as directory:
    Path(directory).chmod(0o700)
    first=allocate();ports=[first,allocate([first])]
    environment=dict(os.environ,SSL_CERT_FILE=str(Path(directory)/"cert.pem"))
    thumb=None
    if sys.platform=="win32":
        root=Path(directory)
        subprocess.run(["openssl","req","-x509","-newkey","ec","-pkeyopt","ec_paramgen_curve:prime256v1","-nodes","-days","1","-subj","/CN=127.0.0.1","-addext","subjectAltName=IP:127.0.0.1","-addext","basicConstraints=critical,CA:FALSE","-addext","extendedKeyUsage=serverAuth","-keyout",str(root/"key.pem"),"-out",str(root/"cert.pem")],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        thumb=prepare_certificate(root/"cert.pem",root/"key.pem")
    try:subprocess.run([binary,directory,*map(str,ports)],env=environment,check=True,timeout=40)
    finally:remove_certificate(thumb)
