# 호환성 검증 매트릭스

59번은 시험 환경과 증거를 구분한다. 최신 전체 판정은 해당 source SHA의 [build 실행](https://github.com/Koreapanda4444/LanLink/actions/workflows/build.yml?query=branch%3Amain)과 `LanLink-linux-x64` artifact의 `lanlink-linux-compatibility.json`을 기준으로 한다. 55번 기준의 [전체 성공 실행](https://github.com/Koreapanda4444/LanLink/actions/runs/41637263036)은 이전 검증 증거이며 새 보안 런타임의 성공을 대신하지 않는다.

| 환경/케이스 | 실제 실행 범위 | 증거/한계 |
|---|---|---|
| 두 Linux NAT 라우터 | 분리된 host 두 개·router 두 개의 kernel namespace, veth·bridge·SNAT/DNAT, 알려진 공인 매핑 candidate | `nat_matrix.py`, `nat_peer`: production signed UDP punch와 양방향 end-to-end 암호화 Direct QUIC |
| NAT + UDP 차단 | 두 router FORWARD에서 UDP DROP, 같은 두 host의 실제 클라이언트 | Direct 후보 실패 뒤 인증된 Relay TLS fallback과 network list 제어 성공. 이 시나리오의 가상 IPv4 데이터 왕복은 별도 Relay 회귀 시험과 구분 |
| cone/restricted/symmetric 모델 | 결정적 candidate 매핑/차단 상태 기계 | `punch_tests`; 실제 공유기의 NAT 동작이나 공인 STUN 성공률을 측정한 것은 아님 |
| Windows x64 | native QUIC/TLS·Direct·암호화·서비스·Wintun TCP/UDP·EXE/ZIP·업그레이드 복구 | Windows Actions 러너의 실제 커널/서비스 시험 |
| Linux x64 | native QUIC/TLS·Direct·TUN/TAP·systemd·IPC·데스크톱 패키지 | Ubuntu Actions 러너, 격리 namespace 및 실제 서비스 설치 |
| macOS arm64 | native QUIC/TLS·Direct·utun·launchd·IPC·앱 패키지 | 실제 arm64 macOS 러너. 앱은 ad hoc 서명, 다른 macOS 버전과 Intel 실행은 미확인 |
| 여섯 OS 방향 | 세 live native peer가 public identity·봉인 키·암호문·Direct proof를 교환 | `platform-interop` action. artifact 교환 방식이며 OS 사이 실시간 UDP 전송은 미확인 |
| Linux Relay 배포 | Oracle Linux 9 빌드, 동일 archive를 Ubuntu 22.04·Debian 12에서 실행 | 설치·업그레이드·롤백·재시작·3-node·WAL 백업/복원 |
| v0.2.2 Relay | hash를 고정한 실제 공개 archive와 새 Linux 클라이언트 | 암호화 unicast/fanout·격리·재시작·추방·용량·TLS fallback 회귀 |
| Teeworlds 0.7.5 | 고정 upstream 소스로 실제 전용 서버를 빌드하고 서버명·버전 query | `game_profile_integration.py`. localhost 프로토콜 검증, VPN을 경유한 물리 host/guest 게임 플레이·검색 목록은 미확인 |

## NAT 시험 재현

Linux에서 CMake로 `lanlink_nat_peer`, `lanlink-relay`를 빌드하고 `iproute2`, `iptables`, Python 3, OpenSSL을 준비한다.

```sh
sudo timeout 120 unshare --net python3 tests/nat_matrix.py \
  build/lanlink_nat_peer build/lanlink-relay /absolute/nat-result.json
```

스크립트는 장치가 `lo`뿐인 새 network namespace에서만 시작한다. 정해진 문서용 주소 203.0.113.0/24와 사설 LAN 10.11.0.0/24·10.12.0.0/24를 사용한다. parent namespace에 외부 인터페이스나 기본 경로를 만들지 않는다. 생성한 자식 namespace는 종료 시 제거하며 개인 fixture는 임시 0700 디렉터리에만 둔다. 공개 artifact에는 결과·kernel·source SHA만 기록한다. 자동 STUN 발견은 이 시험에 포함하지 않는다.

## 최종 승인에 남은 수동 항목

- 실제 두 공유기/공인 NAT·CGNAT·대칭 NAT에서의 Direct 성공률과 fallback·NIC/IP 변경.
- Windows↔Linux↔macOS 물리/VM host 사이 실시간 가상 IP 데이터 왕복.
- 실제 host/guest에서 게임의 직접 IP 접속·LAN 목록 검색·장시간 플레이.
- macOS 14/15 및 Intel, Developer ID·공증·Gatekeeper 다운로드 설치.
- Windows Gateway의 실제 사설 LAN 배포와 다른 OS의 L2 지원 확장.

위 항목은 CI와 소스 구현만으로 완료 처리하지 않는다. v1.0.0 패키지는 준비할 수 있지만 이 실사용 승인 조건은 공개 안정판 배포 전에 별도로 확인해야 한다.
