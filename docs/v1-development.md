# v1.0 개발 로드맵

첨부 최종 기획서의 60개 커밋을 유지한다. 이번 작업 범위는 01~17이다. 구현 상태와 검증 상태를 분리하며 실제 장비에서 확인하지 못한 항목은 검증 결과에서 명시한다. 공개 배포는 별도 승인 후 진행한다.

| 번호 | 목표 | 상태 |
|---|---|---|
| 01 | `refactor(core): separate virtual network, transport and session layers` — 서비스의 패킷·세션·경로 책임 분리, 기존 테스트 결과 동일 | 구현 |
| 02 | `refactor(transport): introduce path-independent connection interfaces` — Relay/P2P 양 경로를 수용하는 공통 송수신 및 lifecycle 인터페이스 | 구현 |
| 03 | `refactor(service): decouple virtual adapter from relay session` — Wintun-패킷 엔진 연결에서 Relay 직접 의존 제거, 기존 패킷 왕복 유지 | 구현 |
| 04 | `feat(protocol): add capability negotiation and protocol versioning` — 구버전 Relay/클라이언트 폴백·지원 기능 불일치 테스트 | 구현 |
| 05 | `feat(diagnostics): extend path telemetry and structured errors` — 상태/RTT/패킷/재전송/경로 선택 사유/오류코드 수집, 민감정보 제외 | 구현 |
| 06 | `chore(build): establish cross-platform build and CI foundation` — 기존 Windows/Linux CI 무손상, 이후 macOS 빌드 진입점 마련 | 구현 |
| 07 | `feat(p2p): add authenticated candidate exchange` — 네트워크 멤버에게만 후보 전달, 다른 네트워크 정보 비노출 | 구현 |
| 08 | `feat(p2p): implement local address candidate gathering` — 다중 NIC/가상 NIC/loopback 예외 처리, 주소 우선순위 | 구현 |
| 09 | `feat(p2p): implement STUN endpoint discovery` — 공인 후보 조회, 실패/타임아웃/응답 유효성 검증 | 구현 |
| 10 | `feat(p2p): implement UDP NAT hole punching` — 서로 다른 NAT 환경 모의 테스트, 제한된 재시도 | 구현 |
| 11 | `feat(p2p): add ICE-style connectivity checks` — 후보 쌍 검사/우선순위/역할 충돌/연결 상태 기계 테스트 | 구현 |
| 12 | `feat(p2p): establish authenticated direct QUIC sessions` — peer 인증·암호화 확인, Relay를 거치지 않는 데이터 흐름 검증 | 구현 |
| 13 | `feat(p2p): bind membership and key epochs to peer sessions` — 추방/키 변경/세션 재생 공격 차단 테스트 | 구현 |
| 14 | `feat(p2p): implement relay fallback and route recovery` — 직접 실패 시 기존 Relay/TLS-TCP까지 자동 복구 | 구현 |
| 15 | `feat(p2p): add path quality sampling and selection` — RTT·손실·상태 기반 선정, 경로 선택 로그·진단 표시 | 구현 |
| 16 | `feat(p2p): implement migration and path revalidation` — IP 변경·NIC 전환·일시 단절 후 세션 복구 및 중복 패킷 처리 | 구현 |
| 17 | `feat(ui): expose direct and relayed peer connection status` — 사용자에게 현재 경로·상태·폴백 사유 노출 | 구현 |
| 18 | `fix(network): audit and harden existing IPv4 broadcast fanout` — 기존 fanout 구현 재사용, 실제 전달·서브넷·유출 차단 검증 | 대기 |
| 19 | `feat(network): extend scoped multicast forwarding` — 그룹 범위·참여/탈퇴·TTL·rate-limit 검증 | 대기 |
| 20 | `feat(discovery): stabilize LAN service discovery over virtual networks` — 범위 한정한 검색 트래픽 라우팅, 중복/폭주 억제 | 대기 |
| 21 | `feat(discovery): add verified game compatibility profiles` — 확인된 게임에 한해 명시적 프로파일/우회책, 근거 문서 | 대기 |
| 22 | `fix(network): improve MTU and IPv4 fragmentation handling` — 경로별 MTU 차이/큰 패킷/체크섬 및 연결 무손상 | 대기 |
| 23 | `fix(network): prevent fanout loops and duplicate injection` — P2P-Relay 전환 시 중복·브로드캐스트 증폭 방지 | 대기 |
| 24 | `feat(network): add optional platform-specific L2 backend` — 실제 TAP/브리지 기반 선택적 L2, 최소 1개 지원 OS에서 실동작·격리 검증; 불가능한 OS는 명시 | 대기 |
| 25 | `feat(ui): add game network diagnostics and compatibility guidance` — LAN 검색 실패 원인/방화벽/라우팅/연결 방법 안내 | 대기 |
| 26 | `feat(identity): introduce optional user account and device enrollment` — 필수 계정 없이 자체 서버 사용 유지, 장치 등록·폐기 지원 | 대기 |
| 27 | `feat(directory): add configurable official and self-hosted bootstrap` — 검증 가능한 Directory API·서버 목록 서명/인증·수동 URL 대체 불가 방지 | 대기 |
| 28 | `feat(network): implement password-protected network joining` — KDF·가입 제한·기존 관리자 승인과 충돌 없는 정책 | 대기 |
| 29 | `feat(invite): add expiring invitation links and one-time codes` — 기한·사용 횟수·권한·회수 기능 | 대기 |
| 30 | `feat(ui): simplify network creation and join workflows` — 방 생성·참여 최소 단계, 네트워크 상태 표시 | 대기 |
| 31 | `feat(ui): add presence, path diagnostics and network overview` — 실시간 멤버 상태·가상 IP 복사·P2P/Relay 표시 | 대기 |
| 32 | `feat(onboarding): implement first-run setup and UI localization` — 기존 설치 언어 기능 유지, 데스크톱 UI 한국어/영어 및 첫 실행 안내 | 대기 |
| 33 | `feat(topology): support managed mesh policies` — 허용 멤버 간 네트워크 경로/권한 정책, P2P·Relay 일치 | 대기 |
| 34 | `feat(topology): add hub-and-spoke restrictions` — spoke 간 통신 제한, hub 차단 시 우회 경로 방지 | 대기 |
| 35 | `feat(gateway): register approved gateway nodes` — 관리자 승인된 gateway만 등록·공지 | 대기 |
| 36 | `feat(gateway): add routed subnet advertisement` — 서브넷 충돌 검출·명시적 경로 설정·통신 및 원복 | 대기 |
| 37 | `fix(gateway): enforce isolated and safe packet forwarding` — IP forwarding·ACL·DNS·라우팅 복구, 인터넷 전체 경로 기본 금지 | 대기 |
| 38 | `feat(acl): enforce per-member and per-network policies` — 거부 기본값/권한 변경 즉시 반영, UI·Relay/P2P 우회 불가 | 대기 |
| 39 | `feat(acl): implement device groups and role assignments` — 관리자·일반 멤버·제한 멤버 차등 권한 | 대기 |
| 40 | `feat(admin): add bounded audit events and troubleshooting` — 주요 관리 작업 이벤트, 민감정보 비기록, 보존 제한 | 대기 |
| 41 | `feat(ui): expose topology, gateway and access rules` — 설정 미리보기·위험 경고·권한별 제어 화면 | 대기 |
| 42 | `refactor(platform): extract OS adapters and service abstractions` — Windows 기존 기능 보존, 네트워크/IPC/서비스 추상화 | 대기 |
| 43 | `feat(linux): implement TUN adapter and IPv4 route management` — Linux TUN, interface up/down, MTU/라우트/정리 | 대기 |
| 44 | `feat(linux): add daemon lifecycle and local IPC` — systemd, 최소 권한·재시작·로그 처리 | 대기 |
| 45 | `feat(linux): package desktop UI and client installer` — Linux x86_64 지원 패키지, UI 플랫폼 백엔드 | 대기 |
| 46 | `feat(macos): implement utun and route management` — macOS utun/권한/경로/정리, macOS 빌드 검증 | 대기 |
| 47 | `feat(macos): add native daemon lifecycle and IPC` — launchd, 권한 경계 및 정리 | 대기 |
| 48 | `feat(macos): package desktop UI and signing workflow` — macOS 앱/패키지, 서명·공증은 자격정보가 있을 때만 실행 | 대기 |
| 49 | `feat(platform): ensure cross-platform peer interoperability` — Windows↔Linux↔macOS 암호화/키/상태/패킷 호환 | 대기 |
| 50 | `feat(relay): register and discover multiple relay regions` — 지역·용량·프로토콜·신뢰 정보 등록, 공식/자체 분리 | 대기 |
| 51 | `feat(relay): implement authenticated health and load signals` — 상태 확인·용량 수치·서버 신뢰 검증, 민감정보 노출 방지 | 대기 |
| 52 | `feat(relay): select relay using health and latency probes` — 지역보다 실측 품질 우선, 변경 안정화·수동 고정 | 대기 |
| 53 | `feat(relay): recover sessions after relay failure` — 인증·네트워크 상태·키 재동기화, 데이터 중복/유실 복구 | 대기 |
| 54 | `feat(relay): add quotas, accounting and abuse safeguards` — 전송량/연결/요청량 제한, 장애·비용 방지 및 운영 메트릭 | 대기 |
| 55 | `feat(deploy): ship reproducible multi-region self-hosting` — Oracle Linux 9 등 기존 배포 경로 유지, 설정·롤백·백업 문서 | 대기 |
| 56 | `fix(reliability): harden lifecycle, cancellation and concurrency` — 재접속·서비스 재시작·경로 전환·종료 시 race/leak/deadlock 제거 | 대기 |
| 57 | `perf(network): optimize buffering, allocations and forwarding` — 측정값 기반 성능 개선, 회귀 지표 제공 | 대기 |
| 58 | `security(audit): verify protocol, auth, keys and permissions` — 위협 모델·악용 방지·의존성 점검·보안 회귀 테스트 | 대기 |
| 59 | `test(compat): validate NAT, games and cross-platform matrices` — 실제 시험 환경 증거·결과 표·미지원 사항 문서화 | 대기 |
| 60 | `release(v1.0.0): prepare and verify production artifacts` — Windows/Linux/macOS 패키지, 체크섬, 업그레이드/롤백, 릴리스 노트; 실제 공개 발행은 승인 후 | 대기 |

01~17의 자동 검증 대상과 실제 환경 제한은 [v0.4 검증 기록](verification-v0.4.md)에 정리한다. 18~60은 이번 개발 범위에 포함하지 않는다.

## 기능별 커밋 SHA

아래는 기획 번호에 대응하는 기능 커밋이다. 플랫폼/CI 수정과 복구 시간 예산 검사는 별도 커밋으로 남겼다. 검증 결과는 해당 기능을 포함한 최종 SHA의 Actions 결과와 함께 확인한다.

| 번호 | GitHub 커밋 |
|---|---|
| 01 | [af31548](https://github.com/Koreapanda4444/LanLink/commit/af315481a98c5a64cc25185660176b087dcfec64) |
| 02 | [bad3a6c](https://github.com/Koreapanda4444/LanLink/commit/bad3a6cee2b4b0d570b535dc2656732c07b6cbe9) |
| 03 | [9e1d934](https://github.com/Koreapanda4444/LanLink/commit/9e1d93413ed02e4de4796910f6f065aac48e967e) |
| 04 | [7dc3e34](https://github.com/Koreapanda4444/LanLink/commit/7dc3e34056cb9344b76c68b366a197b998f42bf7) |
| 05 | [9314a56](https://github.com/Koreapanda4444/LanLink/commit/9314a561db6ca54e0cbc0aae99fe53d063b8c21f) |
| 06 | [63cbf6c](https://github.com/Koreapanda4444/LanLink/commit/63cbf6cf82b6ec1d8028e954a2be91120335f7c1) |
| 07 | [9f5aedd](https://github.com/Koreapanda4444/LanLink/commit/9f5aeddc3346c92912a5db833ae0e29d49fc6045) |
| 08 | [c9e419c](https://github.com/Koreapanda4444/LanLink/commit/c9e419c6eb49eed6ef0b1e3f4e372360573468e9) |
| 09 | [b62d2e8](https://github.com/Koreapanda4444/LanLink/commit/b62d2e8d9deb96fcd8acd475d7ed4dca1e31b73e) |
| 10 | [b1c7533](https://github.com/Koreapanda4444/LanLink/commit/b1c7533f74fd93e1ec166fd31a9f38f2129bfbc0) |
| 11 | [1daaaae](https://github.com/Koreapanda4444/LanLink/commit/1daaaae010f773c2a7ac787935e426747463aff8) |
| 12 | [f7bdca9](https://github.com/Koreapanda4444/LanLink/commit/f7bdca975c7a3c95f561b29efbba18812d59726e) |
| 13 | [3581ec9](https://github.com/Koreapanda4444/LanLink/commit/3581ec9dd11f2dd87a48bc4d8f44306514015b5d) |
| 14 | [911e8b1](https://github.com/Koreapanda4444/LanLink/commit/911e8b1906d301f8159781db3242edac24615429) |
| 15 | [f9a3f39](https://github.com/Koreapanda4444/LanLink/commit/f9a3f3973763cf2590daab13347ce5396a19841c) |
| 16 | [b6aacfd](https://github.com/Koreapanda4444/LanLink/commit/b6aacfd34a4905b367ceccce2c7d8ddd8795c4da) |
| 17 | [bdf4af5](https://github.com/Koreapanda4444/LanLink/commit/bdf4af560b82f4c065e97009b697ce19be00d9a5) |
