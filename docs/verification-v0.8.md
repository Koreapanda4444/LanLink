# v0.8.0 검증 범위 (42~49)

기준 main은 `d25ad109314994dbc3f3cb56d4f7f59a5df39532`이다. 구현과 실제 검증 범위를 구분하며 공개 Release는 발행하지 않는다.

| 번호 | 구현 | 검증 증거 |
|---|---|---|
| 42 | 어댑터 factory, 공통 서비스 실행부, OS 타입이 없는 IPC 인터페이스 | 기존 Linux native 42/42 및 이후 Windows 회귀 CI |
| 43 | Linux TUN, IPv4 주소·MTU·경로와 정리 | 실제 격리 네트워크 namespace에서 TUN 생성·UDP 왕복·경로·제거 통과 |
| 44 | Unix 소켓 IPC, systemd와 foreground daemon | IPC/공통 UI 모델 포함 44/44, SIGTERM·IPC 종료·재시작 통과 |
| 45 | 공유 UI 화면, Linux SDL3/OpenGL, 설치·업그레이드·제거 패키지 | 패키지 실행 의존성·설치·상태 보존·복구·제거, 한·영 캡처 통과 |
| 46 | macOS utun, native IPv4 주소·경로 관리 | 실제 macOS utun 생성·MTU·커널 UDP 왕복·경로·제거, DNS·기본 경로 보존 통과 |
| 47 | macOS native daemon, launchd와 동일 IPC | macOS 전체 44/44, QUIC/TLS 통합, foreground 종료·재시작 및 실제 launchd crash 복구 통과 |
| 48 | macOS 앱·아이콘·라이브러리 재배치·설치 패키지와 서명 절차 | 실제 macOS arm64 앱·의존성·ad hoc 서명 검증, 설치·업그레이드·복구·제거와 한/영 캡처 통과. Developer ID·공증은 미실행 |
| 49 | 세 native OS 간 공개 상태·봉인 키·암호화 패킷·Direct 인증 교환 | CI에 여섯 방향 상호운용 검증을 추가했다. 해당 커밋의 결과 확인 전이다 |

Linux 클라이언트와 패키지 검증은 [실행 41372179796](https://github.com/Koreapanda4444/LanLink/actions/runs/41372179796), macOS utun은 [실행 41373091265](https://github.com/Koreapanda4444/LanLink/actions/runs/41373091265), macOS 전체 런타임과 launchd는 [실행 41374584484](https://github.com/Koreapanda4444/LanLink/actions/runs/41374584484)에 기록되어 있다. macOS 앱 패키지와 전체 회귀는 [실행 41375367906](https://github.com/Koreapanda4444/LanLink/actions/runs/41375367906)의 모든 검증 job에서 통과했다. Linux 한·영 오프라인 메인 화면과 시작 안내 캡처는 수동 검토했다.

## 49번 상호운용 검증

Windows x64·Linux x64·macOS arm64 러너에서 각각 native helper를 실행한다. 각 프로세스가 생성한 장치 ID·X25519 개인 키·그룹 키는 해당 러너에 보관한다. Actions artifact에는 공개 서명 상태와 상대 기기의 공개 키로 봉인한 키, 암호화 패킷, Direct 서명만 올린다. 동일 workflow SHA와 attempt의 세 참여자 자료만 받아 검증한다. 재시도는 모든 job을 함께 실행해야 한다.

- 여섯 OS 방향에서 protocol 5 frame과 64비트 revision/epoch, 공개 키 서명·그룹·역할을 읽는다.
- 수신 OS에서 production X25519 키 복호화, AES-GCM IPv4 1400바이트 왕복과 재전송·변조·epoch·잘못된 수신자 차단을 검증한다.
- Direct 세션 binding과 서명, 정책의 기본 거부 전환을 같은 native crypto/codec/authorization 구현으로 확인한다.
- 별도의 로컬 3-process fixture는 위 교환 순서를 빠르게 검사한다. 서로 다른 OS에서 실행하는 CI 교환과 구분한다.
- Linux와 macOS 설치 패키지는 실제 CI systemd/launchd 경로에도 설치해 그룹 접근·비인가 사용자 차단·강제 종료 후 재시작·장치 ID 보존 업그레이드·기본 제거·명시적 purge를 검증하도록 추가했다. 실행 결과는 해당 CI에서 확인한다.
- 최종 release manifest는 Windows ZIP/EXE, Linux relay 및 Linux/macOS client archive를 모두 포함하고 버전·출처 SHA·native 실행 파일 형식·라이선스·체크섬을 검사한다. 공개 Release 발행 단계는 실행하지 않는다.

이 시험은 artifact를 통해 실제 서로 다른 OS의 암호화·wire 호환성을 검사한다. OS 사이의 실시간 UDP/QUIC 전송이나 실제 NAT를 대신 검증하지 않는다. native QUIC/TLS 및 P2P 회귀는 각 OS의 별도 통합 시험으로 수행한다.

## 보존 및 권한

- 기존 Windows 서비스·Wintun·Win32/DirectX UI·설치·업그레이드 복구 경로를 보존했다. Wire protocol 5와 `lanlink/5` ALPN, 관리형 정책과 키 배포 규칙도 유지한다.
- Unix 제어 소켓은 앱 소유 디렉터리, 0660 소켓, peer credentials 및 동일 프레임 상한을 사용한다. `lanlink` 그룹은 로컬 관리 권한을 가지므로 신뢰하는 사용자만 추가한다.
- Linux systemd는 capability와 파일시스템·장치·주소 계열을 제한한다. macOS utun/경로 변경은 root daemon만 수행한다. GUI는 일반 사용자로 실행한다.
- 설정·장치 ID·토큰은 프로그램 교체와 분리하며 기본 제거는 상태를 보존한다. 사용자 명시 `--purge`만 해당 상태를 삭제한다.
- 종료·연결 해제·권한 회수 시 앱 소유 주소와 경로를 회수한다. 물리 NIC·DNS·인터넷 기본 경로를 변경하지 않는다.

## 제한 및 미확인 항목

- 로컬 작업 환경은 `/dev/net/tun`, network namespace와 Unix 소켓 생성을 허용하지 않는다. 실제 OS 네트워크·IPC·서비스 검증은 GitHub Linux/macOS/Windows 러너에서 수행한다.
- macOS CI와 패키지는 arm64에서 실행했다. Intel x64 Mac에서의 실제 실행은 미확인이다.
- Linux/macOS는 승인된 사설 경로를 사용하는 클라이언트를 지원한다. LAN으로 전달하는 Gateway 역할은 Windows WFP 백엔드가 필요하다. Unix UI에서 이 역할의 로컬 활성화를 비활성화하고 RPC와 설정에서도 거부한다.
- 서로 다른 물리 OS의 PC 사이 실시간 통신, 다양한 실제 NAT·게임, 물리 LAN Gateway 배포는 미확인이다. 여러 물리 장비 검증은 후속 호환성 Phase에서 수행한다.
- 기본 macOS 패키지는 ad hoc 서명이다. 자격정보가 필요한 Developer ID·Apple 공증과 다운로드 후 Gatekeeper 설치 검증은 미실행이다.
