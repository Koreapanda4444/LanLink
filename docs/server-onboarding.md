# 서버 등록과 초대 — 최신 main

설치 → 앱 실행 → 서버 등록 또는 초대 확인 → 기기 등록 → 서버 연결 → 네트워크 생성·가입 순서로 사용한다. 공식 Relay·Directory·클라우드 계정은 필요 없다. 서버 운영자가 관리 비밀을 사용자에게 나눠 주는 방식도 필요 없다. 공개 v1.0.0 프리릴리스는 이 개편 이전이므로 최신 main의 CI 패키지를 사용한다.

## 앱에서 서버 등록

1. 왼쪽 **서버 추가**에서 이름·호스트/IP·포트를 저장한다. 기본 포트는 4433이다. 일반 인증서는 OS/CA 신뢰와 호스트명 검증을 사용한다.
2. 자체 서명 인증서라면 운영자에게 별도 신뢰 경로로 확인한 **SPKI SHA-256 공개키 지문**을 입력하고 확인한다. 인증서 파일 자체의 SHA-256과는 다르다. 서버 주소에 맞는 SAN과 유효 기간도 검증한다.
3. 저장한 서버를 선택하고 **수정 · 기기 등록**에서 운영자의 일회용 등록 코드와 기기 이름을 입력한다.
4. **연결**을 누른다. 등록만으로 연결하지 않는다. 연결이 완료되면 기존 네트워크 생성·ID/비밀번호 가입·초대 가입을 사용한다.

여러 서버를 저장할 수 있으며 활성 연결은 하나다. 이름 수정은 연결을 끊지 않는다. 호스트·포트·신뢰 지문을 바꾸면 해당 프로필의 인증 정보와 기기 identity를 초기화하므로 다시 등록한다. 다른 프로필에는 영향을 주지 않는다. 연결 해제·전환 시 기존 세션과 가상 네트워크를 먼저 종료한다. 자동 재연결도 선택한 서버에만 수행한다.

**기존 개별 인증 정보** 입력은 이미 발급된 기기 인증 정보를 넣는 선택 기능이다. 운영자의 master token을 넣지 않는다. 비워 두면 기존 인증 정보를 유지한다. 일반적인 등록은 코드 방식으로 진행한다.

서버가 없으면 서비스와 GUI만 실행하고 가상 어댑터를 만들거나 접속을 재시도하지 않는다. 선택된 서버의 인증 정보 누락, 인증 실패, 연결 불가, 시간 초과와 인증서 변경 차단은 진단에서 구분한다. 공개키를 고정한 Relay 연결은 호스트·시간·SPKI를 확인하는 TLS 1.3/TCP 경로를 사용한다. 일반 CA 신뢰 연결은 기존 QUIC/UDP와 TLS fallback을 유지하며 Direct P2P·패킷 암호화도 유지한다.

## 운영자: 자체 Relay와 기기 승인

Relay 패키지를 풀고 호스트에서 실행한다. 공개 호스트명/IP를 지정하며 서버 방화벽과 제공자 방화벽에 선택한 TCP/UDP 포트를 허용한다.

```sh
sudo python3 setup-relay.py vpn.example.com --port 4433
```

이 도구는 서버 전용 master secret과 호스트 SAN이 있는 자체 서명 인증서를 만들고 기존 설치기를 실행한다. 새 설치는 `relay_require_device_registration=true`이며 개별 등록 전에는 서버에 접근할 수 없다. 완료 시 5분·1회용 `lanlink://` 서버 초대를 출력한다. 이 초대만 사용자에게 비공개로 전달한다. TLS 개인 키와 master token은 서버에 남는다. `--no-start`는 초대를 발급하지 않으며 `--root DIR`는 오프라인 설치만 수행한다.

공인/자체 CA 인증서를 이미 운영한다면 기존 `install-relay.sh --certificate ... --private-key ... --token-file ...`을 사용한다. 이 토큰도 **서버 관리자 전용**이다. 설치기는 기존 인증·상태 파일을 보존한다. 서비스 계정은 `lanlink-relay`다.

```sh
# 등록 코드: 기본 300초, 허용 범위 1~86400초
sudo -u lanlink-relay /opt/lanlink/bin/lanlink-relay --device-code /etc/lanlink/relay.conf
# 연결 정보·SPKI·일회용 등록 코드를 담은 초대
sudo -u lanlink-relay /opt/lanlink/bin/lanlink-relay --device-invite /etc/lanlink/relay.conf vpn.example.com 4433
# 목록·차단·미사용 코드 철회
sudo -u lanlink-relay /opt/lanlink/bin/lanlink-relay --device-list /etc/lanlink/relay.conf
sudo -u lanlink-relay /opt/lanlink/bin/lanlink-relay --device-revoke /etc/lanlink/relay.conf DEVICE_ID
sudo -u lanlink-relay /opt/lanlink/bin/lanlink-relay --device-code-revoke /etc/lanlink/relay.conf CODE
# 별도 경로로 확인할 공개키 지문
/opt/lanlink/bin/lanlink-relay --certificate-pin /etc/lanlink/server.cert
```

코드의 만료·사용·철회 상태와 기기 승인 목록은 SQLite에 지속 저장한다. 코드는 해시로 저장하고 기기 인증은 기존 Ed25519 identity 서명과 개별 secret의 proof를 함께 확인한다. 철회하면 현재 QUIC/TLS 세션도 닫고 네트워크 권한도 거부한다. 철회한 동일 identity는 새 코드로 다시 승인할 수 없다. 새로운 프로필/identity와 새 운영자 승인이 필요하다. 이 명령은 서버의 로컬 파일에 접근할 수 있는 운영자만 사용한다. 원격 관리자 비밀을 클라이언트에 배포하지 않는다.

기존 수동 구성의 프로토콜 호환성은 보존한다. `relay_require_device_registration=false`는 이전 공용 토큰 방식을 허용하는 명시적 호환 설정이며 신규 배포의 권장 사용자 흐름에는 사용하지 않는다. 이미 개별 등록된 identity와 철회된 identity는 이 모드에서도 master token으로 인증할 수 없다. 지역 앞단에서는 등록 요청도 중앙 authority로 전달하며 기기 승인·철회 명령은 중앙에서 실행한다.

## 서버·네트워크 초대

네트워크 관리자가 기존 네트워크 초대를 발급한 뒤 **서버 및 네트워크 초대 복사**를 누르면 서버 정보와 대상 네트워크를 함께 전달할 수 있다. 상대가 아직 서버에 등록되지 않았다면 운영자에게 받은 별도의 일회용 서버 등록 코드를 선택 입력한다. 네트워크 관리자라는 이유로 서버 등록 코드를 발급할 수는 없다.

운영자는 `--device-invite CONFIG HOST PORT NETWORK_LINK`로 기존 네트워크 초대를 함께 감쌀 수도 있다. 이 작업이 네트워크 초대 권한을 새로 만들지는 않는다. 유효한 네트워크 초대나 정상적인 가입 승인이 계속 필요하다. 서버 등록 코드는 기본 5분·1회용, GUI의 네트워크 초대는 기본 15분·1회용으로 각각 만료·철회된다. 서버만 등록되어도 네트워크 가입 권한은 별도다.

`lanlink://connect/...`를 클릭하면 앱의 확인 창에 서버 주소·공개키·대상 네트워크가 표시된다. **서버 전환 승인과 지문 확인 후에만** 등록·연결·네트워크 가입을 이어간다. Windows URL 등록, Linux desktop MIME handler, macOS bundle URL scheme을 사용한다. 실행 중인 Windows 앱과 macOS 앱도 링크를 확인 창으로 전달받는다. 기존 `lanlink://join/...`와 `LL1...` 네트워크 초대는 현재 연결된 서버에서 계속 사용할 수 있다. 링크의 명령·추가 인수·잘못된 인코딩은 실행하지 않는다.

일시적인 가입 요청 실패는 최대 3회 재시도하며, 응답을 잃었을 때는 현재 가입 상태를 먼저 확인한다. 앱/서비스 종료 시 자동 가입 재시도용 초대는 메모리에서 폐기된다. 서비스 재시작 후에는 저장된 서버와 기기 인증 정보로 재연결하며, 아직 완료하지 않은 초대는 다시 확인해야 한다. 등록 응답을 잃었을 때 같은 기기에 새 코드를 발급할 수 있으며 이미 사용한 코드는 재사용할 수 없다.

## 저장과 로컬 제어

서비스 데이터 디렉터리의 `profiles/servers.state`는 프로필·선택 상태와 별도 보호 파일의 참조만 저장한다. 기기 secret은 Windows DPAPI, Unix의 private 0600 파일로 보호하며 파일·metadata는 기존 원자적 저장 경로를 사용한다. IPC 응답과 GUI/CLI 프로필 목록에 secret은 포함하지 않는다. 주소·신뢰 변경 및 secret 갱신은 새 파일을 먼저 만들고 metadata를 교체한다. 선택된 서버의 연결 세대와 서비스 시작마다 새 세대를 사용하여 전환 전·비정상 종료 전의 네트워크 명령이 다른 연결에 적용되지 않도록 한다.

GUI와 CLI는 OS의 로컬 서비스 접근 권한이 있는 사용자만 사용한다. 이 권한은 프로필 변경·네트워크 관리·서비스 종료를 포함한다. Repair와 일반 재설치는 프로필·기기 secret·사용자 설정을 유지한다. 삭제는 별도 데이터 제거 선택에 따른다. 이전 공개 버전 설정의 자동 마이그레이션은 이번 개발 범위에 포함하지 않는다.

명령줄에서도 같은 서비스 경로를 사용한다. 등록 코드를 터미널 히스토리에 남기지 않으려면 `CODE` 대신 `-`를 지정하고 표준 입력으로 전달한다.

```sh
lanlink-ui-cli server-add Home vpn.example.com 4433 SPKI_SHA256
lanlink-ui-cli server-list
lanlink-ui-cli server-enroll PROFILE_ID - "My PC"
lanlink-ui-cli server-connect PROFILE_ID
lanlink-ui-cli server-disconnect
lanlink-ui-cli server-edit PROFILE_ID Home vpn.example.com 4433 SPKI_SHA256
lanlink-ui-cli server-delete PROFILE_ID
lanlink-ui-cli server-accept 'lanlink://connect/...' --confirm-switch --trust-pin
```

## English quick start

Install without server details. Open **Add server**, save the host/port, verify an optional operator-provided SPKI SHA-256 pin, register with a one-use operator code, then press **Connect**. Create or join networks after server authentication. Multiple isolated profiles are supported, with one active server. Editing an endpoint or pin requires fresh registration. Repair/reinstall preserves profiles and protected credentials.

Self-host with `sudo python3 setup-relay.py HOST --port 4433`. Share the generated short-lived invitation privately. Keep the master token and TLS private key on the relay host. Local `--device-code`, `--device-list`, `--device-revoke` and `--device-code-revoke` manage device access. New installations require individual registration. A server invitation cannot bypass network membership checks. Opening a `lanlink://` link only shows a review dialog; connection and server switching require explicit consent.

Pinned Relay connections use verified TLS 1.3/TCP; normal CA trust retains QUIC with TLS fallback. Direct P2P and end-to-end packet encryption remain enabled. Physical router NAT, mixed-host long gameplay, Windows application signing and Apple notarization remain separate release-validation work.
