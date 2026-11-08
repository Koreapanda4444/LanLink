param([Parameter(Mandatory)] [string] $CertificatePath, [Parameter(Mandatory)] [string] $PrivateKeyPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Certificate fixture requires PowerShell 7.' }
$certificate = $null
function Write-FixturePem([string] $Path, [string] $Label, [byte[]] $Bytes) {
    $encoded = [Convert]::ToBase64String($Bytes, [Base64FormattingOptions]::InsertLineBreaks)
    [IO.File]::WriteAllText($Path, "-----BEGIN $Label-----`n$encoded`n-----END $Label-----`n", [Text.UTF8Encoding]::new($false))
}
try {
    $certificate = New-SelfSignedCertificate -Type Custom -Subject "CN=LanLink fixture $([Guid]::NewGuid())" `
        -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
        -Provider 'Microsoft Software Key Storage Provider' -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature, KeyEncipherment `
        -TextExtension @('2.5.29.19={critical}{text}ca=0', '2.5.29.17={text}IPAddress=127.0.0.1', '2.5.29.37={text}1.3.6.1.5.5.7.3.1') `
        -NotAfter (Get-Date).AddDays(1)
    Write-FixturePem $CertificatePath 'CERTIFICATE' $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert)
    $key = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
    try { Write-FixturePem $PrivateKeyPath 'PRIVATE KEY' $key.ExportPkcs8PrivateKey() }
    finally { $key.Dispose() }
    Write-Output $certificate.Thumbprint
} catch {
    if ($null -ne $certificate) { Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey -Force }
    throw
}
