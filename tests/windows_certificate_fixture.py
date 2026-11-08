"""Register only ephemeral CI server keys required by the Windows Schannel backend."""
import os
from pathlib import Path
import re
import subprocess
import sys

def prepare_certificate(cert,key):
    if sys.platform!='win32':return None
    # Match the existing native Windows harness: a temporary CNG key in My,
    # exported PEM for OpenSSL and a Schannel-readable private key. No root trust is added.
    result=subprocess.run(['pwsh','-NoProfile','-NonInteractive','-File',str(Path(__file__).with_suffix('.ps1')),'-CertificatePath',str(cert),'-PrivateKeyPath',str(key)],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if result.returncode:raise RuntimeError('Windows certificate fixture failed: '+result.stderr)
    thumb=result.stdout.strip()
    assert re.fullmatch('[0-9A-Fa-f]{40}',thumb),result.stdout
    return thumb

def remove_certificate(thumb):
    if thumb is None:return
    assert re.fullmatch('[0-9A-Fa-f]{40}',thumb)
    environment=dict(os.environ,LANLINK_TEST_CERT_THUMB=thumb)
    subprocess.run(['pwsh','-NoProfile','-NonInteractive','-Command',"$ErrorActionPreference='Stop'; Remove-Item -LiteralPath ('Cert:\\CurrentUser\\My\\'+$env:LANLINK_TEST_CERT_THUMB) -DeleteKey"],env=environment,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
