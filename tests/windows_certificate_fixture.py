"""Register only ephemeral CI server keys required by the Windows Schannel backend."""
import os
from pathlib import Path
import re
import subprocess
import sys

def import_certificate(cert,key):
    if sys.platform!='win32':return None
    pfx=Path(cert).with_suffix('.pfx')
    subprocess.run(['openssl','pkcs12','-export','-in',str(cert),'-inkey',str(key),'-out',str(pfx),'-passout','pass:lanlink-ci-ephemeral'],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    environment=dict(os.environ,LANLINK_TEST_CERT_PFX=str(pfx))
    result=subprocess.run(['powershell','-NoProfile','-NonInteractive','-Command',"$ErrorActionPreference='Stop'; $password=ConvertTo-SecureString 'lanlink-ci-ephemeral' -AsPlainText -Force; Import-PfxCertificate -FilePath $env:LANLINK_TEST_CERT_PFX -CertStoreLocation Cert:\\CurrentUser\\My -Password $password -Exportable | Select-Object -ExpandProperty Thumbprint"],env=environment,text=True,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    thumb=result.stdout.strip()
    assert re.fullmatch('[0-9A-Fa-f]{40}',thumb),result.stdout
    pfx.unlink()
    return thumb

def remove_certificate(thumb):
    if thumb is None:return
    assert re.fullmatch('[0-9A-Fa-f]{40}',thumb)
    environment=dict(os.environ,LANLINK_TEST_CERT_THUMB=thumb)
    subprocess.run(['powershell','-NoProfile','-NonInteractive','-Command',"$ErrorActionPreference='Stop'; Remove-Item -LiteralPath ('Cert:\\CurrentUser\\My\\'+$env:LANLINK_TEST_CERT_THUMB) -DeleteKey"],env=environment,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
