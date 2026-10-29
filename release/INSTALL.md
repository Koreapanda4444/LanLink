# LanLink 0.5.0 설치

## 파일 선택

| 파일 | 용도 |
| --- | --- |
| `LanLink-Setup-0.5.0-windows-x64.exe` | Windows 설치 프로그램 |
| `LanLink-0.5.0-windows-x64.zip` | Windows 수동 설치 패키지 |
| `LanLink-relay-0.5.0-linux-x86_64.tar.gz` | Linux x64 릴레이 서버 |

`SHA256SUMS`에서 받은 파일의 해시를 확인한다. PowerShell에서는 `Get-FileHash 파일이름 -Algorithm SHA256`, Linux에서는 `sha256sum --check SHA256SUMS --ignore-missing`을 사용한다. `release-manifest.json`은 해당 빌드의 소스 커밋과 패키지 정보를 담는다.

## 릴레이 서버

특정 호스팅 서비스에 종속되지 않는다. 배포 패키지는 Linux x64, glibc 2.34 이상에서 사용하며 설치기는 systemd가 실행 중인 서버를 요구한다. 동일한 패키지를 Oracle Linux 9, Ubuntu 22.04, Debian 12에서 검증한다. 그 외 환경은 소스에서 빌드할 수 있다.

Ubuntu·Debian의 설치 도구:

```bash
sudo apt-get update
sudo apt-get install -y python3 openssl ca-certificates libstdc++6
```

Oracle Linux·RHEL 계열의 설치 도구:

```bash
sudo dnf install -y python3 openssl ca-certificates libstdc++
```

클라이언트가 접속할 호스트명 또는 IP에 맞는 PEM 인증서와 개인 키를 준비한다. 인증서는 클라이언트 Windows에서 신뢰해야 한다. 사설 CA를 사용한다면 그 CA의 공개 인증서를 클라이언트의 신뢰할 수 있는 루트 저장소에 등록한다. 개인 키는 서버에 보관한다.

공유 토큰 파일을 만들고 허용할 클라이언트에 전달한다.

```bash
umask 077
openssl rand -hex 32 > auth.token
```

릴레이 패키지를 압축 해제한 디렉터리에서 설치한다.

```bash
sudo bash ./install-relay.sh --certificate /absolute/server.cert \
  --private-key /absolute/server.key --token-file /absolute/auth.token
sudo systemctl status lanlink-relay
sudo journalctl -u lanlink-relay -n 50
```

기본 포트는 TCP·UDP 4433이다. 호스팅 제공자의 방화벽·보안 그룹에서도 두 프로토콜을 허용해야 한다. 설치기는 실행 중인 firewalld가 있으면 포트를 설정하며, 다른 방화벽은 변경하지 않는다. UFW를 사용한다면 다음과 같이 허용한다.

```bash
sudo ufw allow 4433/tcp
sudo ufw allow 4433/udp
```

`--port`, `--bind`로 포트와 수신 주소를 지정한다. `--firewall-zone`은 firewalld용이며, `--no-firewall`은 설치기의 방화벽 변경을 생략한다. 프로그램은 `/opt/lanlink`, 설정·인증서는 `/etc/lanlink`, 데이터베이스는 `/var/lib/lanlink`, 로그는 `/var/log/lanlink`에 설치한다. 서비스는 별도 `lanlink` 계정으로 실행한다.

다음 항목은 `/etc/lanlink/relay.conf`에서 설정하며 변경 후 서비스를 재시작한다.

```ini
relay_max_connections=128
relay_max_tls_handshakes=16
relay_send_queue_bytes=4194304
relay_send_queue_frames=256
relay_control_requests_per_second=32
```

연결 한도는 QUIC·TCP와 인증 전 연결에 공통 적용한다. 제어 메시지에는 별도 큐 여유를 남기고, 출력 큐가 가득 차면 데이터 패킷을 버린다. 제어 상태를 갱신할 수 없는 클라이언트는 연결을 종료한 뒤 현재 상태로 재연결한다.

## Windows 클라이언트

설치 시작 시 한국어 또는 영어를 선택한다. 무인 설치에서는 `/LANG=ko` 또는 `/LANG=en`으로 지정할 수 있다.

`LanLink-Setup-0.5.0-windows-x64.exe`를 실행하고 릴레이 주소, 포트와 인증 토큰 파일을 입력한다. 관리자 권한이 필요하다. 기본 프로그램 위치는 `C:\Program Files\LanLink`, 데이터 위치는 `C:\ProgramData\LanLink`다. 필요한 DLL과 서명된 공식 Wintun을 포함하며, LanLink 자체 실행 파일과 설치 프로그램은 서명되지 않았다.

설치가 끝나면 시작 메뉴에서 LanLink를 연다. 네트워크를 만들고 ID를 공유하거나, 전달받은 ID로 가입 요청을 보낸다. 소유자가 승인하면 가상 IP가 표시된다. 게임이나 애플리케이션에서는 상대 기기의 가상 IP를 사용한다. 창을 닫아도 Windows 서비스가 연결을 유지한다.

소유자는 현재 서비스가 받은 가입 요청을 화면에서 승인할 수 있다. 이전 요청은 기기 ID를 받아 `Manage a device by ID`에서 승인할 수도 있다. 네트워크 탈퇴와 기기 제거에는 확인 창이 표시된다.

ZIP 수동 설치는 압축을 해제한 디렉터리의 관리자 PowerShell에서 실행한다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-LanLink.ps1 `
  -RelayHost relay.example.com -AuthenticationTokenFile C:\secure\auth.token
```

## 업그레이드와 제거

새 Windows 설치 EXE를 실행한다. 기존 연결을 유지하려면 릴레이 주소와 토큰 파일을 비워 두고 원래 프로그램·데이터 디렉터리를 사용한다. 수동 설치는 새 패키지의 설치 스크립트를 기존 디렉터리에 대해 다시 실행한다. 릴레이도 새 패키지의 설치기를 다시 실행한다. 설정, 기기 인증 정보와 릴레이 데이터베이스를 유지한다.

Windows는 설치된 앱 목록이나 `C:\Program Files\LanLink\Uninstall-LanLink.ps1`에서 제거한다. 릴레이는 `sudo bash /opt/lanlink/uninstall-relay.sh`로 제거한다. 기본 제거는 데이터를 남긴다. Windows의 `-RemoveData`, 릴레이의 `--remove-data`는 인증 정보와 저장 상태까지 삭제한다.
