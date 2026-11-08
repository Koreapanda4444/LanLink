# LanLink 설치 — 최신 main

이 문서는 설치·서버 연결·개별 인증 개편을 포함한 최신 프리릴리스 패키지 기준이다. 기존 v1.0.0은 개편 전 패키지이며 최신 다운로드는 저장소 README의 링크를 사용한다. 실제 공유기·혼합 OS 실시간 통신·host/guest 게임 시험은 별도 검증으로 남는다. 전체 패키지를 사용하고 프로그램 일부 DLL/so만 교체하지 않는다.

설치 후 앱에서 서버를 등록하고 운영자의 일회용 코드로 기기를 승인받은 뒤 연결한다. 서버 접속과 네트워크 가입 권한은 별개다. 새로 설치한 앱의 서버 미등록 표시는 정상 상태다. 자세한 흐름은 저장소의 `docs/server-onboarding.md`를 따른다.

## 파일 선택

| 파일 | 용도 |
| --- | --- |
| `LanLink-Setup-1.0.0-windows-x64.exe` | Windows 설치 프로그램 |
| `LanLink-1.0.0-windows-x64.zip` | Windows 수동 설치 패키지 |
| `LanLink-relay-1.0.0-linux-x86_64.tar.gz` | Linux x64 릴레이 서버 |
| `LanLink-1.0.0-linux-x64.tar.gz` | Ubuntu 24.04 x64 데스크톱 클라이언트 |
| `LanLink-1.0.0-macos-arm64.zip` | macOS 14 이상 Apple Silicon 클라이언트 |

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

아래 토큰은 서버 관리자 전용 master secret이다. 클라이언트에게 전달하지 않는다. 신규 설치기는 개별 기기 등록을 필수로 설정한다. 자체 서명 인증서를 처음 만드는 운영자는 `sudo python3 setup-relay.py PUBLIC_HOST --port 4433`으로 인증서·관리 secret 생성과 설치를 한 번에 수행할 수도 있다. 완료 시 표시되는 5분·1회용 초대만 사용자에게 비공개로 전달한다.

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

`--port`, `--bind`로 포트와 수신 주소를 지정한다. `--firewall-zone`은 firewalld용이며, `--no-firewall`은 설치기의 방화벽 변경을 생략한다. 프로그램은 `/opt/lanlink`, 설정·인증서는 `/etc/lanlink`, 데이터베이스는 `/var/lib/lanlink`, 로그는 `/var/log/lanlink`에 설치한다. 서비스는 별도 `lanlink-relay` 계정으로 실행한다.

다음 항목은 `/etc/lanlink/relay.conf`에서 설정하며 변경 후 서비스를 재시작한다.

```ini
relay_max_connections=128
relay_max_tls_handshakes=16
relay_send_queue_bytes=4194304
relay_send_queue_frames=256
relay_control_requests_per_second=32
```

연결 한도는 QUIC·TCP와 인증 전 연결에 공통 적용한다. 제어 메시지에는 별도 큐 여유를 남기고, 출력 큐가 가득 차면 데이터 패킷을 버린다. 제어 상태를 갱신할 수 없는 클라이언트는 연결을 종료한 뒤 현재 상태로 재연결한다.

다중 지역 구성·상태 snapshot·복원·순차 롤백은 Relay 패키지의 `self-hosting/README.md`를 따른다. 노드 키를 먼저 만들고 `--node-config`로 실제 공개 키와 대조한다. 지역 앞단에는 중앙 인증 토큰을 복사하지 않는다.

등록 코드는 서버에서 다음과 같이 발급한다. 기기 목록·접속 철회는 `--device-list CONFIG`, `--device-revoke CONFIG DEVICE_ID`를 사용한다.

```sh
sudo -u lanlink-relay /opt/lanlink/bin/lanlink-relay --device-code /etc/lanlink/relay.conf
sudo -u lanlink-relay /opt/lanlink/bin/lanlink-relay --device-invite /etc/lanlink/relay.conf PUBLIC_HOST 4433
```

## Windows 클라이언트

설치 시작 시 한국어 또는 영어를 선택한다. 무인 설치에서는 `/LANG=ko` 또는 `/LANG=en`으로 지정할 수 있다.

Setup EXE를 실행하면 서버 주소·포트·토큰 입력 없이 설치한다. 관리자 권한이 필요하다. 설치 첫 화면은 왼쪽 LanLink 로고와 제품 용도 설명을 표시한다. 설치 위치와 바로가기 옵션을 선택하면 바로 설치한다. 진행 막대는 파일 복사·설정 적용·정리 결과에 따라 0~100%까지 채워지고 모든 작업이 성공한 뒤 100%를 표시한다. 사용자 데이터 위치는 자동 관리하며 별도 선택 화면이나 설치 준비 요약 화면을 표시하지 않는다. 기본 프로그램 위치는 `C:\Program Files\LanLink`, 데이터 위치는 `C:\ProgramData\LanLink`다. 필요한 DLL과 서명된 공식 Wintun을 포함하며, LanLink 자체 실행 파일과 설치 프로그램은 서명되지 않았다.

설치 후 LanLink를 바로 실행할 수 있다. 서버 미등록 시 서비스는 대기하고 GUI는 ‘등록된 서버 없음’을 표시한다. 인증 정보 오류·실제 연결 오류·서비스 실행 오류와 구분한다. 연결 설정을 설치기에서 입력하거나 변경하지 않는다.

이미 설치되어 있으면 같은 EXE에서 다음을 선택한다.

- **Modify:** CLI 도구, 시작 메뉴 바로가기, 바탕 화면 바로가기 변경.
- **Repair:** 선택된 구성은 유지하며 파일·서비스·필수 DLL·방화벽 규칙을 복원. 저장된 서버 프로필·기기 인증 정보·설정은 보존.
- **Uninstall:** 앱과 서비스를 제거. 기본값은 데이터 보존이며 별도 체크로 데이터 삭제를 선택.

Windows ‘설치된 앱’의 수정·제거 진입점은 설치 시 보관한 같은 Setup EXE를 실행한다. 실행 중인 캐시를 덮어쓰지 않도록 관리자 전용 임시 복사본에서 작업한다. 공유 Wintun 드라이버를 전역 삭제하지 않는다. 설치·수정·복구 실패 시 기존 프로그램·서비스 상태·방화벽·바로가기·등록 정보를 복구한다. 파일 잠금으로 복구까지 실패한 경우 백업을 남기고 실패를 알린다.

무인 설치/유지관리 예:

```powershell
.\LanLink-Setup-1.0.0-windows-x64.exe /VERYSILENT /MODE=install
.\LanLink-Setup-1.0.0-windows-x64.exe /VERYSILENT /MODE=modify /Cli=0 /StartMenu=1 /Desktop=1
.\LanLink-Setup-1.0.0-windows-x64.exe /VERYSILENT /MODE=repair
.\LanLink-Setup-1.0.0-windows-x64.exe /VERYSILENT /MODE=uninstall
# 데이터를 삭제할 때만 /RemoveData=1 추가
```

`/NoStart=1`은 서비스 시작을 생략한다. 이전 서버 관련 인수는 받지 않는다. ZIP 수동 설치는 관리자 PowerShell에서 다음과 같이 실행한다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-LanLink.ps1
```

ZIP 자체에는 Setup EXE가 없으므로 설치된 앱의 완전한 EXE 유지관리는 Setup EXE로 설치한 경우 제공한다. ZIP의 수정·복구는 외부에 압축 해제한 패키지의 스크립트를 `-Mode Modify` / `-Mode Repair`로 실행한다.

## Linux·macOS 클라이언트

클라이언트 패키지를 압축 해제한 디렉터리에서 설치한다. Python 3과 관리자 권한이 필요하다. Linux 데스크톱은 systemd·PolicyKit·OpenGL 3.2가 필요하다.

```sh
sudo python3 install-client.py --user YOUR_USER
```

다시 로그인한 뒤 Linux에서는 `/opt/lanlink-client/bin/lanlink-ui`, macOS에서는 `/Applications/LanLink.app`을 연다. `lanlink` 그룹에 추가한 사용자는 로컬 서비스를 관리할 수 있다. GUI는 일반 사용자로 실행한다.

업그레이드는 새 패키지의 `sudo python3 install-client.py`로 수행한다. 기존 설정·장치 ID·토큰을 보존한다. 제거는 Linux의 `sudo python3 /opt/lanlink-client/uninstall-client.py` 또는 macOS의 `sudo python3 "/Library/Application Support/LanLink/uninstall-client.py"`로 실행한다. 기본 제거는 상태를 남기며 명시적 `--purge`만 설정·장치 ID·토큰·로그를 삭제한다.

Linux client는 `/opt/lanlink-client`, `/etc/lanlink-client`, `/var/lib/lanlink-client`를 사용하고 Relay는 기존 `/opt/lanlink`, `/etc/lanlink`, `/var/lib/lanlink`를 사용한다. 같은 호스트에 설치·제거해도 서로의 파일과 서비스를 건드리지 않는다. macOS CI 패키지는 ad hoc 서명이다. Developer ID·Apple 공증과 Gatekeeper 설치 검증은 완료하지 않았다. 자세한 경로·서비스 명령·서명 절차는 각 클라이언트 압축 파일 안의 `INSTALL.md`를 참고한다.

## 업그레이드와 제거

새 Windows 설치 EXE를 실행한다. 원래 프로그램·데이터 디렉터리와 저장된 연결 정보를 유지한다. Repair도 사용자 데이터를 덮어쓰지 않는다. 수동 설치는 새 패키지의 설치 스크립트를 기존 디렉터리에 대해 다시 실행한다. 릴레이도 새 패키지의 설치기를 다시 실행한다. 설정, 기기 인증 정보와 릴레이 데이터베이스를 유지한다.

Windows는 설치된 앱 목록, 같은 Setup의 Uninstall 또는 `C:\Program Files\LanLink\Uninstall-LanLink.ps1`에서 제거한다. 데이터 삭제를 선택하면 서비스 데이터와 현재 사용자 `HKCU\Software\LanLink`의 UI 설정을 삭제하며 다른 사용자의 UI 설정은 임의로 삭제하지 않는다. 릴레이는 `sudo bash /opt/lanlink/uninstall-relay.sh`로 제거한다. 기본 제거는 데이터를 남긴다. Windows의 `-RemoveData`, 릴레이의 `--remove-data`는 인증 정보와 저장 상태까지 삭제한다.

## 데스크톱 첫 실행과 언어

앱은 500×750 크기로 연결 상태와 네트워크 목록을 바로 표시한다. 검정·남색 바탕과 파란 버튼을 사용하며 상단 연결 영역에 LanLink 아이콘을 표시한다. 서버 콤보와 연결·연결 해제 버튼을 사용하고, ‘서버’ 메뉴에서 추가·관리·초대 입력을 연다. 서버 설정의 ‘서버’ 탭에서 주소·신뢰 정보를 저장한 뒤 ‘기기’ 탭에서 일회용 코드로 등록한다. 네트워크를 선택하면 본인의 가상 IP와 멤버·연결 경로를 표시한다. 표시 언어·계정·진단은 ‘도구’ 메뉴에서 연다. 시작 안내나 장식 그림은 표시하지 않는다. 공식 계정이나 중앙 서버는 요구하지 않는다.

비밀번호 방은 생성 창에서 비밀번호와 관리자 승인 여부를 설정한다. 초대 링크는 기본 15분·1회 사용이며 관리자가 남은 링크를 회수할 수 있다. 선택 계정, 서버 기기 등록과 네트워크 가입 정책은 서로 구분한다. 릴레이 데이터베이스는 스키마 9로 자동 마이그레이션되므로 업그레이드 전 백업해야 한다.
