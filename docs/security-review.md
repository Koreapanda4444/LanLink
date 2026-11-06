# 보안 검토와 신뢰 경계

검토일: 2026-11-06. 이 문서는 58번 소스 검토·회귀 시험과 의존성 변경의 범위를 기록한다. 외부 침투 시험이나 모든 취약점이 없다는 인증이 아니다.

## 발견과 수정

- Unix TLS의 `select`/`FD_SET`은 연결 fd가 `FD_SETSIZE` 이상일 때 배열 범위를 벗어날 수 있었다. 단일 fd `poll`로 교체했다. 실제 TLS 수신·송신·backpressure·8개 동시 종료·3회 재시작을 fd 1,024 이상에서 실행한다. Windows는 단일 SOCKET를 넣는 Winsock fd_set 경로를 유지한다.
- 기존 MsQuic 2.4.8은 [ACK underflow](https://github.com/microsoft/msquic/security/advisories/GHSA-gvvw-8j96-8g5r), [경로 처리 use-after-free/RCE](https://github.com/microsoft/msquic/security/advisories/GHSA-92f5-vc22-8j33), [OpenSSL 호스트명 검증](https://github.com/microsoft/msquic/security/advisories/GHSA-w5f4-fx9m-m4q7) 수정 이전 버전이었다.
- overlay port로 MsQuic **2.6.3**, 소스 commit `16ca4d8d785d34fb22285e7c652e477ac16475a5`를 고정했다. Windows는 Schannel, Unix는 고정 vcpkg의 **OpenSSL 3.6.4**를 사용한다. 오래된 내장 QuicTLS 3.1.7을 함께 빌드하지 않는다. OpenSSL의 [3.6.4 보안 변경](https://openssl-library.org/news/openssl-3.6-notes/)도 확인했다.
- Relay·클라이언트·Direct가 MsQuic API를 열 때 실제 런타임 버전을 검사해 2.6.3 미만을 거부한다. 패키지 일부 DLL/so만 교체하는 방식 대신 전체 패키지를 교체한다. 구버전 번호 거부와 정확한 최소 버전 허용을 회귀 검사한다.
- `release/dependencies.json`과 CRLF를 LF로 정규화한 overlay 소스 SHA256을 build-info에 기록한다. 패키지 검증기는 누락·변조된 정책/hash를 거부하며, CI dependency cache 키에 overlay를 포함한다. 기본 vcpkg baseline은 유지한다. SQLite는 해당 baseline의 3.53.4다.

## 위협 모델과 회귀 증거

| 경계/공격자 | 적용된 방어 | 회귀 시험 |
|---|---|---|
| 비인가 원격 연결·인증 proof 재전송 | nonce·장치 서명·토큰 proof·인증 만료·상태 전이·접속 상한 | auth, relay-e2e, network-client |
| 승인되지 않은 peer·폐기된 멤버십 | 네트워크/장치/세션/epoch binding, 정책 lease, 추방·재키잉 | direct-quic, network-client, packet-crypto |
| ciphertext 변조·재전송·다른 네트워크 유입 | AES-256-GCM AAD와 replay window, 검증 후 전달 | packet-crypto, packet-routing, platform-interop |
| 위조 Directory·Relay 상태 | scope·revision·만료·공개 키 고정, TLS·nonce·ID·authority·용량 확인 | directory, directory-https, network-client |
| fanout·느린 reader·요청/트래픽 폭주 | 네트워크 격리·중복 방지·bounded queue·제어 예약·공유 quota | fanout, packet-bridge, transport, network-client |
| 로컬 비인가 사용자 | Windows pipe ACL/secret store, Unix peer credentials·그룹 소켓, 전용 서비스 계정 | native OS 설치/서비스 및 IPC 시험 |
| 자격 자료·백업 노출 | 개인 identity/토큰·credential 권한, private backup·checksum, fenced restore | auth, relay-package, native-install |

OS 관리자·`lanlink` 관리 그룹과 중앙의 멤버십/정책 관리자는 신뢰 경계 안에 있다. 그룹 권한이나 중앙 토큰을 불필요한 사용자에게 배포하지 않는다. TLS는 장치·네트워크 인증을 대체하지 않으며 Direct·Relay 모두 기존 종단 간 패킷 암호화를 유지한다. Directory signing identity는 공개 Directory 서버에 보관하지 않는다.

공개 상태에는 연결 수·용량·제한 계수 등 aggregate만 포함한다. 사용자 payload·네트워크 개인 키·토큰·비밀번호를 metric으로 출력하지 않는다. 물리 NAT·공인 인프라·외부 보안 감사·Developer ID/Apple 공증·모든 전이 의존성의 독립 reachability 감사는 아직 완료 증거가 없다.

## 보고와 운영

민감한 인증정보나 재현용 실제 사용자 데이터를 공개 issue에 올리지 않는다. 운영자는 릴리스마다 upstream MsQuic/OpenSSL 및 OS 보안 공지를 확인하고 전체 패키지를 갱신해야 한다. 고정 버전과 hash는 재현성·출처 확인 수단이며 미래 보안 업데이트를 대신하지 않는다. 알려진 치명적 결함이나 인증 우회가 발견되면 해당 수정과 회귀 검증을 우선한다.
