# 지원 환경

| 구성 | 지원 기준 |
|---|---|
| Windows 클라이언트 | Windows x64, Wintun, Win32/DirectX 11 |
| Linux 클라이언트 | Ubuntu 24.04 x64, systemd, SDL3/OpenGL |
| macOS 클라이언트 | macOS 14+, 패키지 아키텍처 확인, utun |
| Linux Relay | x64, glibc 2.34+, systemd |

기본 패킷 경로는 IPv4와 종단 간 암호화를 사용한다. Direct P2P, Relay QUIC 및 TLS/TCP fallback을 지원한다. 공개키를 고정한 Relay 연결은 TLS 1.3/TCP를 사용한다. Linux L2와 Windows Gateway는 별도 선택 기능이다.

현재 CI는 컴파일과 패키징·의존성·무결성을 확인한다. 자동 기능 회귀 테스트는 저장소에서 제거했다. 실제 공유기 NAT, 혼합 OS의 실시간 연결과 특정 게임의 장시간 실행은 운영 환경에서 별도 확인해야 한다.
