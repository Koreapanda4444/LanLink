# 설치·서버 연결 개편 — Phase 1

이 문서는 Phase 1 완료 시점의 기록이다. 이후 구현된 서버 프로필·개별 등록·초대·공존 설치는 [현재 서버 안내](server-onboarding.md)를 따른다. 아래의 다음 단계 항목을 현재 미구현 상태로 해석하지 않는다.

v1.0.0 프리릴리스 이후의 새 작업 01~04 범위다. 기존 01~60 개발 기록과 별개로 관리한다. 공식 Relay·Directory나 클라우드 계정을 만들지 않는다.

## 구현 범위

| 작업 | 동작 | 주요 파일 |
| --- | --- | --- |
| 01 Windows 설치 | 주소·포트·토큰 페이지와 설치 인수 제거, 앱·자동 서비스·Wintun·런타임 설치, 완료 후 앱 실행 | `packaging/windows/LanLink.iss`, `SetupClient.ps1`, `Install-LanLink.ps1` |
| 02 미설정 상태 | 서버 미등록이면 IPC만 시작, 어댑터·인증 정보 미로드, 정상 종료·재시작, 미등록/인증정보 오류/연결 시도/서비스 오류 구분 | `apps/service/client_runtime.cpp`, `local_control.*`, `apps/ui/desktop_view.hpp` |
| 03 Unix 설치 | 서버 정보 없는 최소 설정 생성, systemd/launchd 정상 시작, 기존 설정 보존, 명시적 제거·purge | `packaging/posix/client_setup.py` |
| 04 Windows 유지관리 | 같은 Setup의 Modify/Repair/Uninstall, 설치된 앱 진입점, 선택 구성 복원, 데이터 삭제 별도 선택, 실패 롤백 | Windows 패키징 스크립트와 `Maintenance-LanLink.ps1` |

Modify에서 실제 변경할 수 있는 항목은 CLI, 시작 메뉴 바로가기, 바탕 화면 바로가기다. 앱·서비스·Wintun·필수 DLL은 필수 구성이다. Repair는 저장된 선택 구성을 유지하며 새 패키지 파일로 복구한다. 별도 보호 디렉터리의 서버 프로필·기기 인증 정보·사용자 설정을 덮어쓰지 않는다. 공유 Wintun 드라이버 제거 API는 호출하지 않는다.

설치된 앱 진입점은 보관된 Setup을 관리자 전용 임시 디렉터리에 복사해 실행한다. 따라서 유지관리 중 실행 중인 자신의 EXE를 삭제하거나 덮어쓰지 않는다. ZIP은 Setup을 포함하지 않으며 외부 패키지의 PowerShell 스크립트로 수정·복구한다. 전체 EXE 유지관리 진입점은 Setup EXE로 설치했을 때 제공한다.

서버 미등록의 진단 값은 `ClientConfigurationState::unconfigured`다. 호스트는 있지만 인증 파일을 읽을 수 없거나 유효하지 않으면 `invalid_credentials`로 표시하고 연결하지 않는다. 서버 미등록에는 재접속 시도나 에러를 만들지 않는다. 유효한 설정 파일 교체를 감지하면 기존 인증·네트워크 경로로 진입하며, 잘못된 중간 설정으로 접속하지 않는다. 활성 연결의 프로필 전환·GUI 등록 IPC는 Phase 2 범위다.

## 자동 검증

- C++ 설정 검증 및 로컬 진단 코덱: 미등록 상태 허용, 연결된 상태와 미등록 상태의 잘못된 혼합 거부.
- Windows ZIP: 서버 없는 서비스 시작, 기존 파일·서비스·ACL·레지스트리·방화벽·바로가기 롤백.
- Windows 실제 Setup EXE: 입력 없는 설치와 UI 실행, Modify 구성 변경, 캐시된 같은 EXE로 손상 DLL/UI/서비스 Repair, 저장 데이터 해시 보존, 실제 파일 잠금 실패 롤백, Uninstall과 데이터 삭제.
- Linux/macOS 서비스: 미등록과 인증정보 오류의 IPC, 터널 미생성, 종료, 기존 구성의 실제 TUN/utun 생성·정리와 identity 보존.
- Linux/macOS 실제 OS 설치: 서버 없는 systemd/launchd 시작과 강제 종료 후 재시작, 관리 그룹 허용/비허용 사용자 차단, 기존 구성 재설치·삭제·purge.
- 기존 암호화·네트워크·Direct/Relay/TLS·kernel NAT 및 패키지 검증을 CI에서 계속 실행한다.

실행 결과는 해당 커밋의 GitHub Actions에서 확인한다. 빌드 성공만으로 네이티브 설치 시험 성공을 대신하지 않는다. 로컬 실행 환경에서는 Unix 소켓 listener가 EPERM으로 차단되므로 해당 IPC 및 실제 시스템 서비스 시험은 네이티브 CI에서 확인한다.

## 다음 범위와 제한

앱 내 서버 추가/수정/삭제, 여러 프로필과 활성 서버 전환, GUI-서비스 설정 동기화, 서버 신뢰 등록은 Phase 2다. 기기별 서버 접근 인증·등록 초대·`lanlink://` 연결은 Phase 3다. 이번 Phase 1에서 이 기능들을 완료했다고 보지 않는다.

Linux 클라이언트와 Relay의 공존 경로 분리는 Phase 4 작업 17에 남긴다. 현재는 같은 호스트에 설치하지 않으며 기존 Relay를 덮어쓰지 않는 검사도 유지한다. Windows purge는 서비스 데이터와 현재 사용자의 UI 설정을 삭제하며 다른 사용자의 UI 설정은 보존한다. macOS 공증, Windows 앱 서명, 실제 물리 NAT·게임 장시간 실행 검증도 별도 범위다.

공개 v1.0.0 Release 파일은 개편 전 소스다. 이 Phase의 main 커밋·CI 아티팩트와 구분한다. 새 Release 게시는 별도 지시 후 진행한다.
