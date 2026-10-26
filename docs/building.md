# 빌드 구성

C++20, CMake 3.24 이상과 `vcpkg.json`의 baseline에 맞는 vcpkg를 사용한다. `VCPKG_ROOT`를 지정하면 CMake Preset으로 플랫폼별 구성을 선택할 수 있다.

| 구성 | 명령 | 범위 |
|---|---|---|
| Windows | `cmake --preset windows-client` | 기존 Win32/DirectX 11 앱·서비스·Relay |
| Linux | `cmake --preset linux-relay` | Linux Relay와 기존 자동 테스트 |
| macOS | `cmake --preset macos-foundation` | 공통 core/protocol/auth/storage/packet engine와 관련 테스트 |

같은 이름으로 `cmake --build --preset 이름`, `ctest --preset 이름`을 실행한다. Windows Ninja 구성은 Visual Studio 개발자 명령 프롬프트에서 사용한다. Windows 배포 패키지는 기존 `LANLINK_PACKAGE_WINDOWS_CLIENT`와 공식 Wintun DLL·라이선스 경로를 지정한다. Linux 배포는 기존 `deploy/linux/build-relay.sh`도 계속 지원한다.

`LANLINK_FOUNDATION_ONLY=ON`은 네트워크 실행 파일을 생략하는 별도 공통 모듈 검사다. 기본값은 OFF이며 Windows·Linux 전체 빌드와 서비스·패키지 검사를 대체하지 않는다. 이 구성으로 macOS 클라이언트, utun, 설치 패키지나 서명·공증이 구현되었다고 주장하지 않는다.

CI는 Windows/Linux 전체 빌드·설치·통신 검사, 실제 v0.2.2 Relay 호환 검사, macOS 공통 모듈 검사를 수행한다. 공개 Release는 `workflow_dispatch`에서 `publish_release`를 명시적으로 선택한 경우에만 발행한다. 일반 push는 검증된 개발 아티팩트만 보관한다.
