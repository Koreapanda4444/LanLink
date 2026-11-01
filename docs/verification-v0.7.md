# v0.7.0 검증 범위 (Commit 33~41)

기준 main은 `a75e41ed08b3c03e598f1bf83a666f2109cb7eb8`이다. 새 기능의 구현과 검증은 별도로 구분한다.

| 항목 | 자동 검증 |
|---|---|
| Mesh / Hub / ACL | 관리형 기본 차단, Relay 전달·수신 복호화·직접 연결 인증의 동일 정책, 방송 격리, epoch 갱신 |
| 기존 직접 세션 | 실제 loopback QUIC 세션에 Hub 차단 정책을 적용해 송수신 권한 제거 |
| Gateway | 장치 신청·관리자 승인, 사설 범위·중복·가상망 충돌, 암호화 왕복, 설정 MTU의 DF 패킷 양방향 전달, 위조·중첩·TTL·ICMP redirect·broadcast 차단 |
| 경로 lifecycle | 로컬 opt-in, 권한 회수·disconnect 시 빈 경로 계획, Windows 실제 경로 추가·삭제 및 전달 flag 복구 |
| Windows forwarding | 두 개의 격리 Wintun 인터페이스 사이의 WFP forward/return, 허용되지 않은 source 차단; DNS와 default route 전후 비교 |
| 역할·그룹 | 관리자 승인·추방 권한, 역할 승격 거부·회수 즉시 반영, 제한 멤버의 재가입 제한 보존 |
| Audit | 구조화 기록, 정책과 원자적 commit, 건수·기간·재시작 보존, 관리자 전용 조회 |
| UI | 설정 preview와 역할별 제어, 한국어·영어 번역 및 Windows 화면 캡처 |

로컬 Linux native 전체 42/42 테스트가 통과했다. 관리형 정책을 QUIC/TLS RPC로 적용해 기존 Direct 세션 종료, 수정 클라이언트의 현재 epoch 패킷 Relay 차단, 명시 허용 후 경로 재수립도 별도 native 통합 테스트에서 통과했다. UI 관리 패널은 ImGui 1.91.9b 헤더로 C++20 구문 검사를 통과했고 한국어·영어 format 검증도 전체 테스트에 포함된다. 최종 코드의 Windows·Linux·macOS CI 결과는 아래에 기록한다. 실물 LAN router/NAT와 여러 PC에서의 Gateway 배포는 미수행이다. Windows forwarding 검증은 격리 가상 인터페이스를 사용하며, Linux/macOS 전체 L3 클라이언트·설치 검증은 다음 Phase 범위다. 공개 Release는 발행하지 않는다.

## 개발 보고

- 기준 main SHA: `a75e41ed08b3c03e598f1bf83a666f2109cb7eb8`
- 이번 Phase: 5 / v0.7.0 / 33~41 구현. 운영 서버·DNS·공개 Release 변경 없음.
- 실제 기능 커밋: [번호별 기록](v1-development.md#기능별-커밋-sha). `main`에만 반영한다.
- 기존 기능 유지 검증: Linux native 전체 42/42, 기존 네트워크·저장소 마이그레이션·QUIC/TLS·P2P·검색·릴리스 자산 검사 통과.
- 새 기능 검증: 정책/역할/Gateway/Audit/미리보기 테스트와 실제 QUIC·TLS 정책 변경 및 Direct 재수립 테스트 통과. Relay 인증을 유지한 채 정책 갱신만 멈춰 권한 만료·복구도 검증했다. 관리형 권한은 6초 이내에 만료하며 최신 정책 확인이 필요하다.
- GitHub Actions: Gateway MTU 수정까지 포함한 코드 [`dcb8d4d`](https://github.com/Koreapanda4444/LanLink/commit/dcb8d4df6ea81c4ea33ed360d871116fa7e653d1)의 [실행 41250872015](https://github.com/Koreapanda4444/LanLink/actions/runs/41250872015)이 성공했다. Windows·Ubuntu native, macOS 공통 모듈, Linux Relay, Debian 12·Ubuntu 22.04 Relay 호환성, v0.7.0 배포 패키지 검사 모두 통과했다. 공개 Release 발행 job은 의도대로 실행되지 않았다.
- 실행하지 못한 실기기 테스트: 여러 물리 PC·실제 LAN router/NAT의 Gateway 배포. 전체 Linux/macOS 클라이언트는 다음 Phase다.
- 보안·안정성 주의: 관리형 정책은 기본 차단, owner online 키 배포 필요, 사설 경로·Gateway는 로컬 opt-in. 물리 LAN의 forwarding·반환 경로는 관리자 사전 구성 필요.
- 다음 작업 번호: 42.

## 최종 코드 검증 증거

- Windows 단위 테스트 41/41 통과. 같은 실행에서 실제 Windows 서비스와 두 Wintun 어댑터를 사용한 QUIC 및 TLS fallback의 TCP/UDP·검색·격리·재시작·실행 중 업그레이드 rollback 시험도 통과했다.
- Windows Gateway 시험에서 사설 경로 추가·삭제, WFP 양방향 전달, 허용되지 않은 source의 기본 차단, forwarding flag 복구를 확인했다. DNS와 default route의 전후 비교도 통과했다.
- 설정 MTU의 DF 패킷을 양방향으로 전달하는 새 회귀 케이스를 포함해 Gateway 테스트가 통과했다.
- 같은 실행의 `LanLink-desktop-screenshots` 캡처 4개를 수동 확인했다. 영어 관리 패널, 한국어 관리 패널·메인 화면·시작 안내의 글자 표시, 줄바꿈, 탭과 주요 버튼 배치를 확인했다. 자동 렌더링·PNG 생성·프로세스 정상 종료 검사도 통과했다. 관리 패널의 나머지 탭에 대한 수동 조작 검증은 미수행이다.
- 여러 물리 PC와 실제 LAN router/NAT를 사용한 Gateway 배포 검증은 미수행이다.
- 문서 반영 후 [실행 41277219094](https://github.com/Koreapanda4444/LanLink/actions/runs/41277219094)은 Windows 단위·설치·Gateway 시험을 통과했으나 통신 시험 준비 중 Relay의 UDP 포트 충돌로 실패했다. 시험 포트 선택이 TCP만 확인하던 문제를 수정해 TCP와 UDP를 함께 확인하며, TLS fallback proxy와 Relay의 포트도 구분한다. 테스트를 건너뛰거나 Relay 오류를 무시하지 않는다.
