# v0.4 개발 검증 기록

대상은 최종 기획서 Commit 01~17이다. 기존 v0.2.2 main에서 시작했으며 기능별 커밋과 CI 수정 커밋을 구분했다. 공개 릴리스는 별도 승인 단계로 남긴다.

| 범위 | 실행 경로/증거 | 구분 |
|---|---|---|
| 기존 네트워크 생성·가입·승인·탈퇴·키 배포 | 기존 core/domain/protocol 및 실제 QUIC/TLS 통합 테스트 | 자동 회귀 검사 |
| v0.2.2 Relay 호환 | 공개 배포본 SHA-256을 고정하고 새 클라이언트로 암호화 전송·재연결·fanout 검사 | 실제 구버전 실행 파일 |
| Direct QUIC | Relay 없이 실제 두 UDP 소켓을 탐색한 후 같은 포트를 넘겨 QUIC 인증·암호화 IPv4·큰 패킷·역방향 전송 | 네이티브 loopback |
| 후보 교환 격리 | 다른 네트워크·위조 발신자·이전 epoch/세션·generation 재사용·구형 기능 협상 거부 | 도메인 및 실제 Relay 통합 |
| NAT 동작 | cone/제한적 mapping, symmetric mapping 불일치, UDP 차단을 분리 | 모의 NAT, 실물 공유기 아님 |
| 경로 복구 | 서비스 경로 선택기 두 개, Direct 전송 시 Relay 데이터 카운터 불변, Relay 복귀 및 중복 거부 | 실제 UDP/QUIC/TLS |
| 멤버·키 폐기 | 멤버 제거, own/peer 세션 교체, epoch 변경, proof nonce/generation 재생 거부 | 자동 검사 |
| 품질 선택 | QUIC 네트워크 패킷 손실 차분과 RTT, 이동평균·hold time·복구 | 실측 수집 + 결정적 정책 검사 |
| 경로 변경 | 임시 Direct 중단 후 복귀, 재검사 중 Relay 전달, NIC 식별 정보 변경 후 재연결, 가상 IP/sequence 유지 | NIC 정보 주입 + 실제 소켓 |
| Windows | 기존 설치·서비스·Wintun·제거/업그레이드 검사, Direct QUIC, UI 모델과 스크린샷 | GitHub Windows runner |
| Linux Relay | 기존 glibc 2.34 패키지 빌드, Ubuntu 22.04/Debian 12 실행 | 기존 CI 유지 |
| macOS | core/protocol/auth/storage/packet engine/P2P 공통 모듈 빌드·검사 | macOS runner, 클라이언트/utun 제외 |

로컬 재현은 `docs/building.md`의 구성을 사용한다. native 전체 CTest와 foundation CTest를 분리한다. `lanlink-network-client`의 Direct 복구 검사는 UDP 검사(최대 5초), QUIC 연결 및 backoff를 포함해 30초의 제한 시간을 가진다. 제어 요청의 기존 8초 제한은 유지하며 전체 시험도 제한 시간 내 종료한다.

[플랫폼별 CI 결과](https://github.com/Koreapanda4444/LanLink/actions/workflows/build.yml)는 각 커밋 SHA의 job/step 결과를 기준으로 확인한다. 실패한 실행을 통과로 집계하지 않는다. 전송 재시도 통계는 MsQuic이 제공하는 손실·재전송 가능한 패킷 수와 구분하며, 실제 재전송 횟수가 없으면 unavailable로 표시한다.

실물 두 공유기/NAT·물리 NIC 변경·실제 게임·Linux/macOS 데스크톱 클라이언트는 이번 환경에서 실행하지 않았다. 후속 호환성 단계의 실제 환경 검증 항목으로 유지한다.
