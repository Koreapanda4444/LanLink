# v0.7.0 검증 범위 (Commit 33~41)

기준 main은 `a75e41ed08b3c03e598f1bf83a666f2109cb7eb8`이다. 새 기능의 구현과 검증은 별도로 구분한다.

| 항목 | 자동 검증 |
|---|---|
| Mesh / Hub / ACL | 관리형 기본 차단, Relay 전달·수신 복호화·직접 연결 인증의 동일 정책, 방송 격리, epoch 갱신 |
| 기존 직접 세션 | 실제 loopback QUIC 세션에 Hub 차단 정책을 적용해 송수신 권한 제거 |
| Gateway | 장치 신청·관리자 승인, 사설 범위·중복·가상망 충돌, 암호화 왕복, 위조·중첩·TTL·ICMP redirect·broadcast 차단 |
| 경로 lifecycle | 로컬 opt-in, 권한 회수·disconnect 시 빈 경로 계획, Windows 실제 경로 추가·삭제 및 전달 flag 복구 |
| Windows forwarding | 두 개의 격리 Wintun 인터페이스 사이의 WFP forward/return, 허용되지 않은 source 차단; DNS와 default route 전후 비교 |
| 역할·그룹 | 관리자 승인·추방 권한, 역할 승격 거부·회수 즉시 반영, 제한 멤버의 재가입 제한 보존 |
| Audit | 구조화 기록, 정책과 원자적 commit, 건수·기간·재시작 보존, 관리자 전용 조회 |
| UI | 설정 preview와 역할별 제어, 한국어·영어 번역 및 Windows 화면 캡처 |

로컬 Linux native 전체 42/42 테스트가 통과했다. 관리형 정책을 QUIC/TLS RPC로 적용해 기존 Direct 세션 종료, 수정 클라이언트의 현재 epoch 패킷 Relay 차단, 명시 허용 후 경로 재수립도 별도 native 통합 테스트에서 통과했다. UI 관리 패널은 ImGui 1.91.9b 헤더로 C++20 구문 검사를 통과했고 한국어·영어 format 검증도 전체 테스트에 포함된다. Windows 실행 결과와 최종 Actions 링크는 작업 완료 보고에서 제공한다. 실물 LAN router/NAT와 여러 PC에서의 Gateway 배포는 이 환경에서 수행하지 않는다. Windows forwarding 검증은 격리 가상 인터페이스를 사용하며, Linux/macOS 전체 L3 클라이언트·설치 검증은 다음 Phase 범위다. 공개 Release는 발행하지 않는다.

## 개발 보고

- 기준 main SHA: `a75e41ed08b3c03e598f1bf83a666f2109cb7eb8`
- 이번 Phase: 5 / v0.7.0 / 33~41 구현. 운영 서버·DNS·공개 Release 변경 없음.
- 실제 기능 커밋: [번호별 기록](v1-development.md#기능별-커밋-sha). `main`에만 반영한다.
- 기존 기능 유지 검증: Linux native 전체 42/42, 기존 네트워크·저장소 마이그레이션·QUIC/TLS·P2P·검색·릴리스 자산 검사 통과.
- 새 기능 검증: 정책/역할/Gateway/Audit/미리보기 테스트와 실제 QUIC·TLS 정책 변경 및 Direct 재수립 테스트 통과. Relay 인증을 유지한 채 정책 갱신만 멈춰 권한 만료·복구도 검증했다. 관리형 권한은 6초 이내에 만료하며 최신 정책 확인이 필요하다.
- GitHub Actions: 해당 커밋의 [워크플로우 실행](https://github.com/Koreapanda4444/LanLink/actions)에서 Windows·Linux·macOS 결과를 확인한다.
- 실행하지 못한 실기기 테스트: 여러 물리 PC·실제 LAN router/NAT의 Gateway 배포. 전체 Linux/macOS 클라이언트는 다음 Phase다.
- 보안·안정성 주의: 관리형 정책은 기본 차단, owner online 키 배포 필요, 사설 경로·Gateway는 로컬 opt-in. 물리 LAN의 forwarding·반환 경로는 관리자 사전 구성 필요.
- 다음 작업 번호: 42.

UI 검증은 CI의 자동 렌더링·PNG 생성·프로세스 정상 종료 검사다. 캡처의 수동 시각 검토와 여러 물리 PC를 사용한 Gateway 배포 검증은 미수행이다.
