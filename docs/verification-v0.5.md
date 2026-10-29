# v0.5 검증 범위

기획서 Commit 18~25의 범위다. 기존 v0.4의 Direct QUIC·Relay 폴백 검증과 v0.2.2 Relay 호환 시험을 유지한다. 일반 push는 공개 Release를 발행하지 않는다. 해당 소스 SHA의 [Actions 결과](https://github.com/Koreapanda4444/LanLink/actions)를 최종 빌드·패키지 결과와 함께 확인한다.

| 범위 | 자동 검증 |
|---|---|
| 18 broadcast | 가상 서브넷·송신자 검증, 다른 네트워크 격리, 기존 fanout 한도 |
| 19 multicast | 지원 범위·IGMP·Windows OS 가입/탈퇴 상태·수신 인터페이스 |
| 20 LAN 검색 | mDNS/LLMNR/SSDP 범위·UDP 형식·속도 제한·중복 억제 |
| 21 게임 프로파일 | 고정된 Teeworlds 0.7.5 전용 서버에 토큰/검색 질의·정상 서버명 응답 |
| 22 MTU | 큰 패킷·IPv4 옵션·fragment offset/MF·DF/ICMP 및 체크섬 |
| 23 중복/루프 | 기존 Direct/Relay 공통 재생 방어·Relay fanout nonce/sequence·TTL 변화 반사 |
| 24 Linux Ethernet | 실제 TAP 두 개·별도 network namespace·암호화 ARP/커널 UDP 왕복·이름/서브넷 충돌 거부·장치 정리 |
| 25 게임 진단 | 제한된 로컬 IPC 직렬화·실제 fanout 카운터·Windows 서비스/UI 모델·구형 서비스 폴백 |

24번의 실제 Linux TAP 시험은 [79dad74의 Linux CI](https://github.com/Koreapanda4444/LanLink/actions/runs/40886608688)에서 실행한다. 이 시험은 커널 장치와 ARP/UDP, 기존 패킷 암호화 엔진을 결합하며 암호화 구간은 Unix datagram 채널이다. Relay/QUIC 및 Windows Wintun은 기존 별도 통합 시험으로 검사한다. 로컬 개발 환경에서는 network namespace 생성 권한이 없어 TAP 통합 시험을 실행하지 못했으며, 이를 mock 성공으로 대체하지 않는다.

## 실제 사용상의 범위

- Teeworlds 전용 서버의 질의 응답은 실제 게임 그래픽 클라이언트 간 플레이나 모든 게임의 LAN 목록 검색 성공을 의미하지 않는다.
- 다른 물리 PC·공유기 NAT·게임 그래픽 클라이언트와 물리 NIC 전환은 별도 실환경 검증이 필요하다.
- Linux Ethernet은 ARP와 제한된 IPv4에 한정한다. 일반 L3 모드와 혼용하지 않는다. 물리 NIC 브리지·VLAN·IPv6 및 Windows/macOS L2는 제공하지 않는다.
- 게임 진단은 현재 인증된 네트워크 상태와 실제 검색 패킷 엔진의 수치를 읽는다. 방화벽·게임 프로세스 상태를 자동 측정하거나 원인을 확정하지 않는다.
- Linux/macOS 일반 데스크톱 클라이언트는 42~49번 범위다. 이번 Linux L2 실행 파일은 해당 전체 클라이언트의 완성을 뜻하지 않는다.
