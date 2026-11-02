# Linux 클라이언트

Linux x64 패키지의 지원 기준은 Ubuntu 24.04다. Python 3, systemd, PolicyKit, OpenGL 3.2 및 데스크톱 세션이 필요하다. 다른 배포판은 소스 빌드 후 의존성과 glibc 호환성을 확인한다. 서버용 Relay 패키지와 클라이언트 패키지는 별개다.

압축을 푼 디렉터리에서 다음과 같이 설치한다. 토큰은 파일로 전달하며 명령행이나 로그에 넣지 않는다.

```sh
sudo python3 install-client.py --relay relay.example.com --port 4433 --token-file /absolute/auth.token --user YOUR_USER
```

다시 로그인하면 `lanlink` 그룹의 사용자가 GUI와 CLI를 통해 서비스를 제어할 수 있다. 이 그룹은 방 생성·참여·관리와 서비스 종료 권한을 가지므로 신뢰하는 사용자만 추가한다. GUI는 일반 사용자로 실행한다.

```sh
/opt/lanlink/bin/lanlink-ui
/opt/lanlink/bin/lanlink-ui-cli status
sudo systemctl status lanlink-client.service
```

설정은 `/etc/lanlink/client.conf`, 장치 ID와 토큰은 `/var/lib/lanlink`, 로그는 `/var/log/lanlink`에 보관한다. 서비스는 제한된 capability와 systemd 보호 옵션을 사용하며 로그인 없이 재시작된다. 종료·권한 회수 시 앱 소유 TUN 주소와 경로를 회수한다. 물리 NIC·DNS·인터넷 기본 경로는 변경하지 않는다.

업그레이드는 새 패키지의 `sudo python3 install-client.py`로 실행한다. 기존 설정·장치 ID·토큰을 보존하며 실패 시 프로그램 파일을 복원한다. `--no-start`는 설치 후 시작을 생략한다. `--root DIR`는 패키지 검증용 staging이며 호스트 서비스나 사용자 그룹을 변경하지 않는다.

```sh
sudo python3 /opt/lanlink/uninstall-client.py
```

제거는 설정과 장치 ID를 보존한다. 사용자가 명시적으로 `--purge`를 지정한 경우에만 해당 클라이언트의 설정·상태·로그를 삭제한다.

Windows와 같은 L3 IPv4 암호화·P2P·Relay 경로와 한국어/영어 화면을 사용한다. Linux TAP은 기존의 별도 선택 모드다. 승인된 사설 경로의 클라이언트 사용은 지원하지만, LAN으로 전달하는 Gateway 역할은 Windows WFP 백엔드가 필요하다. Linux에서 시스템 전체 forwarding이나 방화벽을 임의로 바꾸지 않는다.
