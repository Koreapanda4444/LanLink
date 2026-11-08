# 제품 빌드

C++20, CMake 3.24 이상, 저장소 vcpkg baseline과 일치하는 vcpkg가 필요하다. `VCPKG_ROOT`를 지정한 뒤 사용한다.

| 플랫폼 | 구성 | 빌드 |
|---|---|---|
| Windows x64 | `cmake --preset windows-client` | `cmake --build --preset windows-client` |
| Linux Relay | `cmake --preset linux-relay` | `cmake --build --preset linux-relay` |
| macOS 14+ | `cmake --preset macos-client` | `cmake --build --preset macos-client` |

Windows는 Visual Studio 개발자 명령 프롬프트에서 구성한다. EXE/ZIP 패키지에는 `LANLINK_PACKAGE_WINDOWS_CLIENT=ON`과 공식 Wintun DLL·라이선스 경로가 필요하다. Linux 데스크톱은 `LANLINK_BUILD_DESKTOP=ON`으로 구성한다. Linux Relay 패키지는 `bash deploy/linux/build-relay.sh`로 생성한다. Linux/macOS 클라이언트 패키지는 `lanlink_client_archive` 빌드 대상을 사용한다.

자동 테스트와 벤치마크는 저장소에서 제거했다. CI는 Windows·Linux·macOS 제품을 빌드하고 설치 패키지와 의존성 provenance, 무결성 및 체크섬을 확인한다. 설치 백엔드·서비스·통신 코드가 바뀌면 Windows 설치·수정·복구·제거와 진행률 표시를 실제 실행하여 확인한다. UI·설명만 바뀐 push는 실행 테스트를 생략하고 빌드·패키징만 진행한다. 일반 네트워크 기능 회귀 검증을 대신한다고 해석하지 않는다.
