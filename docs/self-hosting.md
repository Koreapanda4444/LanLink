# 다중 리전 자체 서버 운영

v1.0.0의 지역 Relay는 하나의 영구 중앙 authority를 공유한다. 중앙만 SQLite DB와 클라이언트 인증 토큰을 보관한다. 각 지역 앞단은 별도 TLS 인증서와 상태 서명 identity를 보관하고 원래 클라이언트의 인증 proof·제어·암호화 데이터를 중앙으로 중계한다. 지역 노드마다 독립 DB를 복사하는 방식은 사용하지 않는다. Direct P2P 데이터는 기존대로 Relay를 우회한다. 지역 상행은 TLS/TCP이므로 중앙까지의 지연과 TCP 대기 비용이 있다.

패키지 지원 기준은 Linux x64/glibc 2.34다. 동일 패키지를 Oracle Linux 9, Ubuntu 22.04, Debian 12에서 검사한다. 호스트에는 Python 3.9 이상, OpenSSL, ca-certificates, libstdc++와 systemd가 필요하다. 기존 `install-relay.sh`와 서비스 계정·방화벽 정책을 그대로 사용한다. Linux client와 Relay는 설치 경로를 공유하므로 서로 다른 호스트에 설치한다.

## 1. 패키지와 키 준비

검증된 ZIP/EXE/Linux/macOS 패키지는 CI의 `LanLink-1.0.0-release` artifact에 함께 보관된다. 자체 서버에는 `LanLink-relay-1.0.0-linux-x86_64.tar.gz`를 사용한다. `SHA256SUMS`와 `release-manifest.json`의 소스 SHA를 확인하고 중앙·지역 모두 같은 패키지를 사용한다. 소스 빌드는 `deploy/linux/build-relay.sh`가 `vcpkg.json`의 고정 baseline을 확인한다.

Oracle Linux 9의 런타임 도구:

```sh
sudo dnf install -y python3 openssl ca-certificates libstdc++
```

Ubuntu/Debian:

```sh
sudo apt-get update
sudo apt-get install -y python3 openssl ca-certificates libstdc++6
```

각 호스트에서 패키지를 먼저 압축 해제한다. 아직 설치기를 실행하지 않아도 된다. 각 노드의 실제 호스트명/IP에 맞는 TLS 인증서와 개인 키를 준비한다. 중앙 인증서는 지역 노드가, 지역 인증서는 클라이언트가 신뢰해야 한다. 운영 환경에서 호스트명·인증서 검증을 끄지 않는다. 사설 CA의 테스트·운영 환경은 신뢰할 CA를 OS/OpenSSL에 등록하거나 서비스 환경의 `SSL_CERT_FILE`로 지정한다.

노드별 상태 identity를 **각 호스트에서 별도로** 만든다. 아래의 `relay_id`는 해당 노드 ID, `relay_authority`는 모든 노드의 동일한 authority ID로 바꾼다.

```sh
cat > health-init.conf <<'EOF'
tls_certificate_file=/etc/lanlink/server.cert
tls_private_key_file=/etc/lanlink/server.key
relay_id=home
relay_authority=my-home
relay_health_identity_file=/var/lib/lanlink/relay-health.identity
EOF
sudo install -d -m 0750 /var/lib/lanlink
sudo ./bin/lanlink-relay --health-key health-init.conf
```

이 명령은 개인 키를 유지하며 공개 키만 출력한다. 출력한 64자리 공개 키를 inventory에 기록한다. 중앙과 모든 지역의 개인 identity를 서로 복사하지 않는다. 설치기는 기존 identity를 서비스 계정에 맞게 소유권만 조정한다. 중앙의 클라이언트 인증 토큰은 기존 값을 보존하거나 중앙에서 `umask 077; openssl rand -hex 32 > auth.token`으로 처음 한 번 생성한다. 지역에는 이 토큰을 전달하지 않는다.

## 2. 오프라인 배포 구성 생성

관리용 Linux 머신에서 패키지의 `self-hosting/inventory.example.json`을 복사해 실제 노드 ID·DNS·지역·포트·용량·공개 키를 채운다. authority는 정확히 하나다. 기본적으로 지역만 클라이언트 목록에 나오며, 중앙도 직접 접속 대상으로 공개하려면 중앙의 `advertise`를 `true`로 설정한다. 최대 32개 endpoint를 광고한다. 중앙의 연결 용량은 모든 지역에서 들어오는 연결을 합친 상한이므로 함께 산정한다. quota 설정은 [서버 탐색·제한 문서](directory.md)의 기본값 또는 inventory의 `limits`를 사용한다.

```sh
umask 077
python3 self-hosting/generate.py inventory.json rollout-001 \
  --relay ./bin/lanlink-relay --signer ./bin/lanlink-directory-sign \
  --identity /secure/offline-directory.identity
(cd rollout-001 && sha256sum --check SHA256SUMS)
```

개인 directory signing identity는 출력 디렉터리 밖의 보호된 관리 저장소에 보관한다. 생성 도구는 서버·SSH·클라우드 API에 접속하지 않는다. 동일 inventory는 동일 노드 설정을 만든다. 서명된 catalog의 발행·만료 시각은 실행 시각에 따라 달라진다. `deployment.json`은 도구 버전·inventory hash·공개 키·revision·유효 기간·파일 hash·설치 순서를 기록한다. 새 출력 디렉터리를 사용하며 기존 배포 자료를 덮어쓰지 않는다.

출력에는 다음이 들어간다.

| 경로 | 내용 |
|---|---|
| `NODE/relay.conf` | 해당 노드의 설치용 설정 |
| `NODE/health-public-key.txt` | 실제 private identity와 대조할 공개 키 |
| `relays.bin` | 서명된 self-hosted LLD2 catalog |
| `client-bootstrap.conf` | 클라이언트 설정에 병합할 Directory 항목 |
| `deployment.json`, `SHA256SUMS` | 구성·버전·무결성 기록 |

개인 키·토큰·DB는 출력에 들어가지 않는다. public catalog의 `capacity`는 실제 설정과 같아야 한다. topology, 노드 공개 키, 호스트·포트가 바뀌면 revision을 증가시킨다. 도구는 `--check-config`로 설정을 검사하고 `--verify`로 생성한 catalog의 서명·크기·유효 기간을 확인한다.

## 3. 중앙부터 설치

검증한 노드 설정 디렉터리와 해당 호스트의 TLS 자료를 안전하게 전달한다. 중앙:

```sh
sudo bash ./install-relay.sh --node-config /absolute/rollout-001/home/relay.conf \
  --certificate /absolute/home.cert --private-key /absolute/home.key \
  --token-file /absolute/auth.token
sudo systemctl status lanlink-relay
```

지역 노드:

```sh
sudo bash ./install-relay.sh --node-config /absolute/rollout-001/seoul/relay.conf \
  --certificate /absolute/seoul.cert --private-key /absolute/seoul.key
sudo systemctl status lanlink-relay
```

설치 후 관리 머신 또는 해당 호스트에서 다음과 같이 서버의 서명·신뢰·준비 상태를 검사한다. 공개 키·ID·용량은 inventory의 값을 사용한다. `ready=false`는 종료 코드 2이며, 서명·TLS·접속 오류는 종료 코드 1이다.

```sh
./bin/lanlink-relay --probe kr.example.com 4433 seoul my-home 64 HEALTH_PUBLIC_KEY
```

`--node-config`는 같은 디렉터리의 공개 키와 이미 만든 `/var/lib/lanlink/relay-health.identity`를 대조한다. 누락·불일치 시 프로그램이나 기존 설정을 교체하지 않는다. 기존 ID·authority 또는 중앙/지역 역할을 바꾸는 업그레이드는 거부한다. 지역 설치에 `--token-file`을 주어도 거부한다. 지역은 중앙의 클라이언트 토큰과 로컬 DB를 사용하지 않는다.

기본 포트는 TCP/UDP 4433이다. 지역의 수신 포트는 제공자 방화벽에서도 두 프로토콜을 허용한다. 중앙의 TCP 수신은 지역 노드에서 접근 가능해야 한다. 중앙도 광고한다면 클라이언트의 TCP/UDP 접근이 필요하다. 설치기는 실행 중인 firewalld의 지정 포트만 관리한다. 다른 방화벽·DNS·클라우드 보안 그룹은 운영자가 설정한다. `--no-firewall`은 설치기의 방화벽 수정을 생략한다. generated config의 포트와 다른 `--port`는 거부한다. `--no-start`와 `--root /absolute/staging`으로 시작을 생략하거나 호스트에 영향을 주지 않는 설치 검사를 할 수 있다.

서비스 계정 `lanlink`, `NoNewPrivileges`, `ProtectSystem=strict`, 제한된 capability, 자동 재시작 정책은 기존 unit에 유지한다. 상태는 `/var/lib/lanlink`, 설정·TLS 자료는 `/etc/lanlink`, 로그는 `/var/log/lanlink`에 보관한다.

## 4. catalog와 클라이언트 배포

중앙과 지역이 준비된 뒤 **공개 `relays.bin`만** Directory HTTPS 서버에 전달한다. 관리 키는 이 서버에 두지 않는다. 기존 읽기 전용 서버를 사용할 수 있다.

```sh
python3 self-hosting/serve.py --catalog /srv/lanlink/relays.bin \
  --cert /absolute/directory.cert --key /absolute/directory.key
./bin/lanlink-directory-sign --verify DIRECTORY_PUBLIC_KEY /srv/lanlink/relays.bin
```

클라이언트의 기존 설정·장치 identity·토큰은 보존하고 `client-bootstrap.conf`의 세 항목만 병합한다. 클라이언트에는 중앙에서 사용하는 인증 토큰을 전달한다. `directory_relay_id`를 지정하면 해당 노드로 고정한다. 수동 접속 설정도 유지한다.

catalog는 1시간 유효하다. 보호된 관리 머신에서 **30분 이내 주기**로 새 rollout 디렉터리에 재생성하고 HTTPS 서버의 공개 파일을 임시 파일→rename으로 교체한다. 같은 inventory의 갱신은 같은 revision을 유지할 수 있다. topology·키·endpoint 변경은 revision을 올린다. 갱신이 멈추면 만료된 catalog로 새 연결하지 않으므로 발행 작업·유효 기간을 운영 모니터링에 포함한다. 제공된 도구가 scheduler나 원격 업로드를 자동 등록하지는 않는다.

## 5. 상태 백업과 복원

설치된 기본 경로를 사용하는 중앙과 지역에서 다음 명령을 실행한다.

```sh
sudo python3 /opt/lanlink/self-hosting/state.py backup \
  /etc/lanlink/relay.conf /secure/backups/home-001
sudo python3 /opt/lanlink/self-hosting/state.py verify /secure/backups/home-001
```

중앙 DB는 [SQLite Online Backup API](https://www.sqlite.org/backup.html)를 통해 snapshot을 만든다. 실행 중인 DB 파일과 WAL을 단순 복사하지 않는다. `integrity_check`와 파일 hash를 확인하고, 설정·자격 파일이 백업 도중 바뀌면 실패한다. 기본 제한 시간은 30초이며 큰 DB는 `--timeout`으로 최대 300초까지 지정한다. DB와 자격 자료의 백업 폴더는 0700, 파일은 0600이다. 이 백업에는 토큰과 개인 키가 포함되므로 공개 배포 자료와 분리된 보호 저장소에서 관리한다. 지역 백업에는 중앙의 토큰·DB가 없다. 로그와 클라이언트 장치 identity는 복사하지 않는다.

복원 순서:

1. 중앙의 다른 writer/이전 호스트를 차단하고 해당 Relay 서비스를 정지한다. 중앙 DB를 두 활성 프로세스에 복제해 운영하지 않는다.
2. 기존 `/etc/lanlink`와 `/var/lib/lanlink`를 보호된 롤백 위치로 옮겨 보관한다. 기존 디렉터리가 남아 있으면 복원 도구가 거부한다. 프로그램은 이 단계에서 지우지 않는다.
3. 검증된 백업을 복원한다.
4. 백업에 기록된 버전에 맞는 패키지를 설치·검증한 뒤 시작한다.

```sh
sudo systemctl stop lanlink-relay
sudo python3 /opt/lanlink/self-hosting/state.py restore /secure/backups/home-001 --fenced
sudo bash /absolute/matching-package/install-relay.sh --no-start
sudo systemctl start lanlink-relay
sudo journalctl -u lanlink-relay -n 50
```

`--fenced`는 모든 writer를 정지했다는 운영자의 명시다. 실제 호스트에서는 systemd 상태도 검사한다. 도구는 기존 파일을 덮어쓰지 않으며, manifest의 정해진 설정·상태 파일만 복원한다. 이전 DB의 WAL/SHM이 새 snapshot과 섞이지 않도록 이전 상태 **디렉터리 전체**를 별도로 보관한다. 복원 도중에는 서비스를 시작하지 않는다. 완전히 비어 있는 새 호스트에서는 `--root`로 미리 복원하고 패키지를 설치해 서비스 계정과 권한을 갖춘다.

## 6. 순차 업그레이드와 롤백

현재 구성과 상태를 백업하고 새 패키지 checksum을 확인한다. 중앙을 먼저 갱신하고, 지역은 하나씩 갱신한다. 각 단계에서 서비스 상태·인증·기존 네트워크 ID와 가상 IP·키 재동기화를 확인한 다음 새 catalog를 발행한다. 노드 키 파일은 재생성하지 않는다. 같은 버전의 설치기를 다시 실행하면 설정·토큰·DB·상태 identity를 보존한다. 설치 중 오류가 나면 기존 프로그램·unit·설정·자격 파일을 복원한다.

버전 롤백은 별도 보호 위치에 둔 이전 패키지를 사용한다. 서비스 정지와 writer 차단 뒤, 해당 버전과 함께 만든 상태 백업으로 복원하고 이전 패키지를 `--no-start`로 설치한 다음 시작한다. DB 스키마가 바뀐 경우 새 DB를 이전 바이너리에 그대로 열지 않는다. Directory revision은 낮추지 말고, 이전 endpoint가 필요하면 **더 높은 revision의 새 catalog**로 발행한다. 마지막으로 네트워크와 IP lease가 유지되는지 확인한다.

중앙 authority의 분산 DB 합의·active-active HA·무중단 DB failover는 제공하지 않는다. 지역 앞단 장애는 다른 지역 선택과 재인증으로 복구한다. 중앙 장애는 같은 DB·토큰·상태 identity를 가진 하나의 중앙을 복구하는 절차가 필요하다. 실제 다중 리전 인프라 생성·유료 자원·DNS 변경·공개 Release 발행은 이 도구가 실행하지 않는다.
