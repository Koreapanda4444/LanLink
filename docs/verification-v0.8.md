# v0.8.0 검증 범위 (42~49)

기준 main은 `d25ad109314994dbc3f3cb56d4f7f59a5df39532`이다. 구현과 실제 검증 범위를 구분하며 공개 Release는 발행하지 않는다.

| 번호 | 구현 | 검증 증거 |
|---|---|---|
| 42 | 어댑터 factory, 공통 서비스 실행부, OS 타입이 없는 IPC 인터페이스 | 기존 Linux native 42/42 및 이후 Windows 회귀 CI |
| 43 | Linux TUN, IPv4 주소·MTU·경로와 정리 | 실제 격리 네트워크 namespace에서 TUN 생성·UDP 왕복·경로·제거 통과 |
| 44 | Unix 소켓 IPC, systemd와 foreground daemon | IPC/공통 UI 모델 포함 44/44, SIGTERM·IPC 종료·재시작 통과 |
| 45 | 공유 UI 화면, Linux SDL3/OpenGL, 설치·업그레이드·제거 패키지 | 패키지 실행 의존성·설치·상태 보존·복구·제거, 한·영 캡처 통과 |
| 46 | macOS utun, native IPv4 주소·경로 관리 | 실제 macOS utun 생성·MTU·커널 UDP 왕복·경로·제거, DNS·기본 경로 보존 통과 |
| 47 | macOS native daemon, launchd와 동일 IPC | macOS 전체 44/44 (49번 최종 회귀 45/45), QUIC/TLS 통합, foreground 종료·재시작 및 실제 launchd crash 복구 통과 |
| 48 | macOS 앱·아이콘·라이브러리 재배치·설치 패키지와 서명 절차 | 실제 macOS arm64 앱·의존성·ad hoc 서명 검증, 설치·업그레이드·복구·제거와 한/영 캡처 통과. Developer ID·공증은 미실행 |
| 49 | 세 native OS 간 공개 상태·봉인 키·암호화 패킷·Direct 인증 교환 | Windows x64·Linux x64·macOS arm64 사이 여섯 방향의 봉인 키·암호화 패킷·Direct 서명과 거부 정책 검사 통과 |

Linux 클라이언트와 패키지 검증은 [실행 41372179796](https://github.com/Koreapanda4444/LanLink/actions/runs/41372179796), macOS utun은 [실행 41373091265](https://github.com/Koreapanda4444/LanLink/actions/runs/41373091265), macOS 전체 런타임과 launchd는 [실행 41374584484](https://github.com/Koreapanda4444/LanLink/actions/runs/41374584484)에 기록되어 있다. macOS 앱 패키지와 전체 회귀는 [실행 41375367906](https://github.com/Koreapanda4444/LanLink/actions/runs/41375367906)의 모든 검증 job에서 통과했다. Linux 한·영 오프라인 메인 화면과 시작 안내 캡처는 수동 검토했다.

49번 전체 기능 검증은 [소스 901a988](https://github.com/Koreapanda4444/LanLink/commit/901a9883a6167754c813307c00abc21510446e92)의 [실행 41382287125](https://github.com/Koreapanda4444/LanLink/actions/runs/41382287125)에서 모든 검증 job이 통과했다. 공개 발행 job은 의도대로 건너뛰었다. Windows ZIP/EXE, Linux Relay, Linux client 및 macOS arm64 client의 다섯 실제 패키지에 대해 출처 SHA·실행 파일 형식·필수 파일·라이선스·체크섬을 검증했다. 이후 최소 macOS 빌드 버전 보완도 [main의 최신 build 결과](https://github.com/Koreapanda4444/LanLink/actions/workflows/build.yml?query=branch%3Amain)에서 같은 검증을 수행한다.

| 시험 | 실제 러너 | CTest 결과 |
|---|---|---|
| Windows client | Windows Server 2025 x64 | 42/42 |
| Linux client | Ubuntu 24.04.5 x64 | 45/45 |
| macOS 전체 client | macOS 26.6.2 arm64 | 45/45 |
| macOS 공통 모듈 | macOS 26.6.2 arm64 | 38/38 |

Windows는 실제 서비스·두 Wintun 어댑터의 커널 TCP/UDP·QUIC/TLS fallback·Discovery·격리·재시작·실행 중 업그레이드 복구도 통과했다. Ubuntu 22.04와 Debian 12에서도 같은 Linux Relay 패키지를 검증했다. Linux와 macOS의 한·영 화면 캡처를 수동 검토했으며 한국어 글꼴·로고·시작 안내·서비스 연결 안내의 배치를 확인했다.

## 49번 상호운용 검증

Windows x64·Linux x64·macOS arm64 러너에서 각각 native helper를 실행한다. 각 프로세스가 생성한 장치 ID·X25519 개인 키·그룹 키는 해당 러너에 보관한다. Actions artifact에는 공개 서명 상태와 상대 기기의 공개 키로 봉인한 키, 암호화 패킷, Direct 서명만 올린다. 동일 workflow SHA와 attempt의 세 참여자 자료만 받아 검증한다. 재시도는 모든 job을 함께 실행해야 한다.

- 여섯 OS 방향에서 protocol 5 frame과 64비트 revision/epoch, 공개 키 서명·그룹·역할을 읽는다.
- 수신 OS에서 production X25519 키 복호화, AES-GCM IPv4 1400바이트 왕복과 재전송·변조·epoch·잘못된 수신자 차단을 검증한다.
- Direct 세션 binding과 서명, 정책의 기본 거부 전환을 같은 native crypto/codec/authorization 구현으로 확인한다.
- 별도의 로컬 3-process fixture는 위 교환 순서를 빠르게 검사한다. 서로 다른 OS에서 실행하는 CI 교환과 구분한다.
- Linux와 macOS 설치 패키지는 실제 CI systemd/launchd 경로에도 설치해 그룹 접근·비인가 사용자 차단·강제 종료 후 재시작·장치 ID 보존 업그레이드·기본 제거·명시적 purge를 검증했고 두 OS 모두 통과했다.
- 최종 release manifest는 Windows ZIP/EXE, Linux relay 및 Linux/macOS client archive를 모두 포함하고 버전·출처 SHA·native 실행 파일 형식·라이선스·체크섬을 검사한다. 공개 Release 발행 단계는 실행하지 않는다.

이 시험은 artifact를 통해 실제 서로 다른 OS의 암호화·wire 호환성을 검사한다. OS 사이의 실시간 UDP/QUIC 전송이나 실제 NAT를 대신 검증하지 않는다. native QUIC/TLS 및 P2P 회귀는 각 OS의 별도 통합 시험으로 수행한다.

## 보존 및 권한

- 기존 Windows 서비스·Wintun·Win32/DirectX UI·설치·업그레이드 복구 경로를 보존했다. Wire protocol 5와 `lanlink/5` ALPN, 관리형 정책과 키 배포 규칙도 유지한다.
- Unix 제어 소켓은 앱 소유 디렉터리, 0660 소켓, peer credentials 및 동일 프레임 상한을 사용한다. `lanlink` 그룹은 로컬 관리 권한을 가지므로 신뢰하는 사용자만 추가한다.
- Linux systemd는 capability와 파일시스템·장치·주소 계열을 제한한다. macOS utun/경로 변경은 root daemon만 수행한다. GUI는 일반 사용자로 실행한다.
- 설정·장치 ID·토큰은 프로그램 교체와 분리하며 기본 제거는 상태를 보존한다. 사용자 명시 `--purge`만 해당 상태를 삭제한다.
- 종료·연결 해제·권한 회수 시 앱 소유 주소와 경로를 회수한다. 물리 NIC·DNS·인터넷 기본 경로를 변경하지 않는다.

## 검증 중 보완

- Linux 최소 capability 서비스의 기본 그룹을 `lanlink`로 지정해 제어 소켓 그룹 설정을 허용했다.
- root 소유·비쓰기 가능 runtime 디렉터리를 확인한 뒤 탐색 권한을 설정한다. 개인 파일용 umask 때문에 macOS 제어 소켓에 접근할 수 없었던 문제를 수정했다. 소켓 자체는 그룹 전용 0660을 유지한다.
- Darwin의 사용자 전환 시험에는 기본 그룹과 검증된 제어 그룹만 전달한다. 실제 일반 사용자 접속과 비인가 사용자 거부를 확인했다.
- launchd unload 완료를 기다리고, job이 없는 동안 발생하는 bootstrap EIO에 한해 짧고 제한된 재시도를 수행한다. 실제 업그레이드 후 시작·제거·재설치를 검증했다.
- 클라이언트 설치기·제거기는 기존 프로그램이 client인지 확인한다. 기존 Relay를 모사한 staging에서 설치와 purge가 모두 거부되며 Relay 파일이 보존되는 것을 검증했다. 현재 Linux client와 Relay는 경로가 겹치므로 별도 호스트를 사용한다.
- 상호운용 자료 수집의 일회성 API 실패를 추적할 오류 기록과 제한된 재시도를 추가했다. artifact 조회는 15초, job 상태 확인은 60초 간격이며 동일 workflow SHA·attempt 확인을 유지한다.
- macOS 의존성도 14.0 배포 대상으로 빌드하도록 전용 vcpkg triplet과 캐시 키를 사용한다. 패키지 생성기는 모든 native 바이너리의 Mach-O 최소 버전이 앱 선언을 초과하면 실패한다. 이 검사는 이전 캐시의 macOS 26 의존성을 macOS 14 앱에 섞는 것을 방지한다.
- 최초 49번 실행에서 기존 Linux QUIC/TLS 통합 시험이 한 차례 120초를 초과했다. 실패 단계 진단 출력을 추가했고 후속 전체 실행들은 통과했다. 시간 초과의 근본 원인은 확정하지 못했다.

## 제한 및 미확인 항목

- 로컬 작업 환경은 `/dev/net/tun`, network namespace와 Unix 소켓 생성을 허용하지 않는다. 실제 OS 네트워크·IPC·서비스 검증은 GitHub Linux/macOS/Windows 러너에서 수행한다.
- macOS CI와 패키지는 26.6.2 arm64에서 실행했다. 빌드 최소 버전은 14.0이며, macOS 14·15 등 다른 버전과 Intel x64 Mac에서의 실제 실행은 미확인이다.
- Linux/macOS는 승인된 사설 경로를 사용하는 클라이언트를 지원한다. LAN으로 전달하는 Gateway 역할은 Windows WFP 백엔드가 필요하다. Unix UI에서 이 역할의 로컬 활성화를 비활성화하고 RPC와 설정에서도 거부한다.
- 서로 다른 물리 OS의 PC 사이 실시간 통신, 다양한 실제 NAT·게임, 물리 LAN Gateway 배포는 미확인이다. 여러 물리 장비 검증은 후속 호환성 Phase에서 수행한다.
- 기본 macOS 패키지는 ad hoc 서명이다. 자격정보가 필요한 Developer ID·Apple 공증과 다운로드 후 Gatekeeper 설치 검증은 미실행이다.
