# 설치·서버 연결 개편의 추가 검증

최신 main은 기존 네트워크·어댑터·상호운용·NAT 시험을 유지하면서 서버 프로필 저장/분리, 원자적 인증 정보 저장, 등록 코드 만료·재사용·철회, 실제 TLS 등록·호스트/공개키 검증·실시간 접속 철회 시험을 추가했다. `tests/server_workflow_tests.py`는 Windows Wintun·Linux TUN·macOS utun의 실제 서비스와 두 Relay를 사용하여 추가·등록·전환·해제·초대·비정상 종료 후 재접속·서버 키 변경 차단·기기 차단을 확인한다. Unix에서는 이전 연결 세대의 IPC 명령 거부도 확인한다. 이 시험은 root/관리자와 실제 가상 어댑터가 필요하므로 플랫폼별 CI native 단계에서 실행한다.

`tests/coexistence_tests.py`는 검증된 Linux client/Relay 실제 패키지를 모두 설치한다. 서버만·클라이언트만·두 설치 순서, 재설치 중 상대 파일 보존, 각각의 데이터 제거를 확인한다. Windows 설치 시험은 URL 등록·Repair 복구·소유 handler 제거까지 포함한다. 기존 설치/Modify/Repair/제거·파일 잠금·rollback·서명된 공식 Wintun 보존 검증은 유지한다.

새 소스의 성공 여부는 해당 커밋의 필수 CI 결과를 확인한다. 공개 v1.0.0 프리릴리스 파일을 최신 main 시험 결과로 판단하지 않는다. 실제 물리 공유기와 혼합 호스트 장시간 게임 플레이, Windows 앱 서명, Apple 공증은 계속 별도 검증 단계다.

---

# v1.0.0 릴리스 준비와 검증 범위

56~60의 구현·자동 검증·패키지 준비를 기록한다. 실제 공유기/NAT, 혼합 OS 실시간 가상 IP 왕복, 물리 host/guest 게임 시험은 아직 완료되지 않았다. 공개 안정판 발행 게이트는 닫혀 있다. 이전 검증 기록은 [v0.9](verification-v0.9.md), 운영 범위는 [호환성 표](compatibility-matrix.md)를 참고한다.

| 번호 | 반영 내용 | 확인 경로 |
|---|---|---|
| 56 | TLS 수신기 start/stop 직렬화, 중복 시작 거부, 활성 연결 종료·3회 재시작·8-thread stop | network-client 실제 TLS 회귀, 기존 Direct in-flight 종료·재접속·서비스 시험 |
| 57 | 고정 헤더 생성으로 인증용 payload 복사와 할당 제거 | byte/암호화/상호운용 회귀, [10,000회×5표본 비교](performance.md), Linux CI CSV |
| 58 | 높은 fd의 TLS poll, MsQuic 2.6.3·OpenSSL 3.6.4, 실제 런타임 버전 하한·보안 의존성 provenance | [보안 검토](security-review.md), fd 1,024 이상 TLS 시험, 구버전 runtime 거부·패키지 정책 변조 거부 |
| 59 | 두 kernel NAT 라우터의 signed punch/암호화 Direct와 UDP DROP 후 TLS fallback 시험 도구 | `tests/nat_matrix.py`, CI compatibility JSON. 물리 장비·게임·혼합 OS 실시간 시험은 별도 대기 |
| 60 | 다섯 1.0.0 패키지·버전·출처·라이선스·체크섬 검증, 수동 승인 증거가 없으면 발행 거부 | `verify-release`, `release/readiness.json`, 공개 발행의 사전 차단 시험 |

## 로컬 증거

TLS 동시 종료/재시작과 기존 Direct·Relay 회귀를 통과했다. 새 MsQuic 2.6.3과 OpenSSL 3.6.4를 실제로 빌드해 인증·암호화·Direct·Relay·TLS·상태 복구를 실행했다. 초기 Direct 단독 실행은 설치된 MsQuic의 로컬 라이브러리 검색 경로가 시스템 OpenSSL을 선택해 실패했으며, 개발용 MsQuic 설치의 RPATH를 새 OpenSSL 위치에 맞춘 뒤 통과했다. 이 로컬 경로는 배포 소스나 패키지에 넣지 않는다.

전체 로컬 CTest는 45개를 실행한다. Unix socket 생성 권한이 없는 환경에서 IPC와 UI network model 두 개는 `Operation not permitted`로 실행되지 않으며 나머지 native 회귀는 통과했다. 이 두 시험은 CI에서 생략하지 않는다. Release validator와 공개 승인 차단의 Python 시험은 18개다.

NAT helper의 prepare·서명된 UDP 탐색·양방향 암호화 Direct는 서로 다른 두 프로세스의 loopback에서 확인했다. 로컬에는 `unshare --net` 권한이 없어 NAT topology는 CI의 Linux root namespace에서 실행한다. 59번 소스의 [NAT 검사 단계](https://github.com/Koreapanda4444/LanLink/actions/runs/42135577326/job/127622937577)는 두 kernel NAT 라우터의 Direct 암호화 통신과 UDP 차단 후 인증된 TLS fallback 검사를 통과했다. loopback 실행을 실제 NAT 성공으로 표기하지 않는다.

## CI 판정과 패키지

최종 판정은 이 문서를 포함한 source SHA의 [build 실행](https://github.com/Koreapanda4444/LanLink/actions/workflows/build.yml?query=branch%3Amain)에서 확인한다. 모든 필수 job이 통과해야 패키지 검증을 완료로 인정한다. Windows x64·Ubuntu x64·macOS arm64 native 회귀, 실제 OS 서비스·커널 어댑터·설치·업그레이드·롤백·제거, 여섯 방향 암호화 상호운용을 유지한다. Oracle Linux 9에서 생성한 Relay archive를 Ubuntu 22.04·Debian 12에서도 검사한다.

`LanLink-1.0.0-release` artifact에는 Windows ZIP/EXE, Linux Relay, Linux client, macOS arm64 client 및 `release-manifest.json`, `SHA256SUMS`, `INSTALL.md`가 들어간다. `build-info.json`의 source SHA·clean 상태·protocol 5·baseline·보안 dependency policy·MsQuic overlay hash를 검사한다. 패키지 일부 라이브러리만 교체하지 않는다.

Windows 의존성 초기 검증에서 새 MsQuic의 MSVC 분석 경고가 `/WX`로 오류가 되고 기본 정적 CRT 선택이 vcpkg와 달랐다. overlay에서 CRT를 triplet에 맞추고 `/analyze`·`/W4` 경고는 유지하면서 경고의 오류 승격만 해제했다. 애플리케이션 테스트나 보안 runtime 검사는 완화하지 않았다.

## 공개 발행과 남은 승인

`release/readiness.json`은 실제 공유기/NAT·혼합 OS 실시간 통신·물리 host/guest 게임 시험에 `passed`와 확인 가능한 evidence가 있어야 공개 발행을 허용한다. 현재 세 항목은 모두 pending이다. 발행 도구는 이 게이트를 GitHub 쓰기 전에 확인하며 기존 Release/tag를 자동 삭제하지 않는다. 모든 필수 CI 통과와 해당 실사용 증거를 갖춘 뒤에도 실제 공개 Release는 별도 승인 후 실행한다.

macOS는 ad hoc 서명이며 Developer ID·공증은 미실행이다. Intel 및 macOS 최소 버전 실기기 실행, 공인 인프라 생성·운영, 중앙의 active-active HA는 이번 자동 검증의 완료 범위가 아니다. 이런 제한 때문에 패키지 준비 완료를 제품의 모든 실사용 조건 검증 완료로 해석하지 않는다.

패키지 보안 overlay hash는 checkout의 CRLF/LF 차이를 정규화한 Git 소스 내용의 SHA256이다. Windows 원시 파일 hash와 Linux 원시 파일 hash를 비교해 발생한 최종 validator 불일치를 수정했으며, 줄바꿈 동등성과 실제 버전 변조 거부를 함께 검사한다.
