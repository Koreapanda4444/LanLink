# Linux 클라이언트

Linux x64 패키지의 지원 기준은 Ubuntu 24.04다. Python 3, systemd, PolicyKit, OpenGL 3.2 및 데스크톱 세션이 필요하다. 다른 배포판은 소스 빌드 후 의존성과 glibc 호환성을 확인한다. 서버용 Relay 패키지와 클라이언트 패키지는 별개다. Linux client와 Relay는 별도 프로그램·설정·상태 경로와 서비스로 같은 호스트에 설치할 수 있다. Relay는 별도의 `lanlink-relay` 계정과 그룹을 사용하며 클라이언트 제어 그룹은 Relay 관리 비밀을 읽을 수 없다.

아래 내용은 설치 구조 개편을 적용한 최신 main의 패키지 기준이다. 공개 v1.0.0 프리릴리스는 개편 전 패키지다. 압축을 푼 디렉터리에서 서버 정보 없이 설치한다.

```sh
sudo python3 install-client.py --user YOUR_USER
```

다시 로그인하면 `lanlink` 그룹의 사용자가 GUI와 CLI를 통해 서비스를 제어할 수 있다. 이 그룹은 방 생성·참여·관리와 서비스 종료 권한을 가지므로 신뢰하는 사용자만 추가한다. GUI는 일반 사용자로 실행한다.

```sh
/opt/lanlink-client/bin/lanlink-ui
/opt/lanlink-client/bin/lanlink-ui-cli status
sudo systemctl status lanlink-client.service
```

설치기는 서버를 선택하지 않은 최소 설정을 생성하고 서비스를 시작한다. 서비스는 서버가 없으면 IPC만 제공하며 대기하고 TUN/utun이나 인증 정보를 로드하지 않는다. 앱에서 서버 추가·수정·삭제, 등록 코드 입력, 연결·해제·전환을 진행한다. [서버 등록·초대 안내](https://github.com/Koreapanda4444/LanLink/blob/main/docs/server-onboarding.md)를 따른다. Linux MIME handler와 macOS bundle이 `lanlink://` 초대를 확인 창으로 전달하며 승인 전에는 연결하지 않는다.

설정은 `/etc/lanlink-client/client.conf`, 서버별 identity·개별 인증 정보는 `/var/lib/lanlink-client`, 로그는 `/var/log/lanlink-client`에 보관한다. 서비스는 제한된 capability와 systemd 보호 옵션을 사용하며 로그인 없이 재시작된다. 종료·권한 회수 시 앱 소유 TUN 주소와 경로를 회수한다. 물리 NIC·DNS·인터넷 기본 경로는 변경하지 않는다.

업그레이드는 새 패키지의 `sudo python3 install-client.py`로 실행한다. 기존 설정·장치 ID·토큰을 보존하며 실패 시 프로그램 파일을 복원한다. `--no-start`는 설치 후 시작을 생략한다. `--root DIR`는 패키지 검증용 staging이며 호스트 서비스나 사용자 그룹을 변경하지 않는다.

```sh
sudo python3 /opt/lanlink-client/uninstall-client.py
```

제거는 설정과 장치 ID를 보존한다. 사용자가 명시적으로 `--purge`를 지정한 경우에만 해당 클라이언트의 설정·상태·로그를 삭제한다.

Windows와 같은 L3 IPv4 암호화·P2P·Relay 경로와 한국어/영어 화면을 사용한다. Linux TAP은 기존의 별도 선택 모드다. 승인된 사설 경로의 클라이언트 사용은 지원하지만, LAN으로 전달하는 Gateway 역할은 Windows WFP 백엔드가 필요하다. Linux에서 시스템 전체 forwarding이나 방화벽을 임의로 바꾸지 않는다.

소스 빌드는 `-DLANLINK_BUILD_DESKTOP=ON`으로 SDL3/OpenGL UI를 활성화한다. vcpkg의 SDL3 의존성에는 `autoconf`, `autoconf-archive`, `automake`, `libtool`, `libltdl-dev`가 필요하다. CI의 Linux build tools 단계에 지정된 X11·Wayland·OpenGL·IME 개발 패키지도 설치한다. 빌드 후 `cmake --build build --target lanlink_client_archive`로 패키지를 생성한다.

# macOS 클라이언트

macOS 14 이상을 대상으로 현재 CI 러너의 아키텍처에 맞는 ZIP을 만든다. 파일명의 `arm64`와 `x64`를 구분한다. 다른 아키텍처의 실기기 실행은 검증 결과에서 별도로 확인한다.

ZIP을 푼 디렉터리에서 Linux와 같은 설치 명령을 사용한다. Python 3이 필요하다. 설치 후 `/Applications/LanLink.app`을 연다. 앱의 원본과 서비스는 `/Library/Application Support/LanLink`, 설정은 `/Library/Preferences/LanLink/client.conf`, 서버별 identity·개별 인증 정보는 `/Library/Application Support/LanLinkData`, 로그는 `/Library/Logs/LanLink`에 있다. 앱 링크는 관리 대상 프로그램 디렉터리만 가리킨다.

```sh
sudo python3 install-client.py --user YOUR_USER
sudo launchctl print system/com.lanlink.client
```

`launchd`가 서버 미등록 상태에서도 서비스를 시작하고 비정상 종료 후 재시작한다. 정상 종료·제거 시 utun과 앱 소유 경로를 회수한다. 관리 그룹 권한, 업그레이드 시 상태 보존, `--no-start`, staging `--root`, 제거와 명시적 `--purge`는 Linux와 같다.

기본 CI 패키지는 배포 인증서가 없는 ad hoc 서명이며, Developer ID 서명·Apple 공증·Gatekeeper 설치 검증을 완료했다고 보지 않는다. 이 인증서와 기존 notarytool Keychain 프로필이 준비된 운영자는 아래 절차로 서명과 공증을 수행할 수 있다. 자격정보를 저장소에 넣지 않는다. 명시적으로 실행하기 전에는 이 절차가 시작되지 않는다.

```sh
python3 packaging/macos/sign_and_notarize.py build/macos-client/portable-client signed-output --identity "Developer ID Application: YOUR_IDENTITY" --notary-profile YOUR_PROFILE
```

위 절차는 중첩 바이너리·앱 서명, DMG 생성·서명, notarytool 제출, ticket staple·검증을 수행한다. 기본 CI는 이 절차를 실행하지 않는다. 자체 인증서를 쓰는 Relay의 테스트·개발 환경에서는 `SSL_CERT_FILE`로 신뢰할 CA 파일을 명시할 수 있다. 인증서와 호스트 이름 검증은 유지된다.
