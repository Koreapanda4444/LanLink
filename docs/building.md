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

## 보안 런타임과 최종 검증

기본 baseline 외에 저장소의 `cmake/ports/msquic` overlay를 적용한다. MsQuic 2.6.3 commit을 고정하며 Unix는 vcpkg OpenSSL 3.6.4, Windows는 Schannel을 사용한다. 시스템 OpenSSL을 직접 지정하는 개발 빌드도 3.6.4 이상이 필요하다. 실행 시 MsQuic 2.6.3 미만은 거부한다. overlay를 제외하거나 예전 DLL/so를 섞지 않는다.

macOS 전체 client는 `deploy/macos/build-client.sh`, Linux 전체 client는 CI의 `LANLINK_BUILD_DESKTOP=ON` 경로를 사용한다. 두 플랫폼의 실제 daemon·커널 어댑터·설치 검사는 별도 CI 단계에서 수행한다. `LANLINK_BUILD_BENCHMARKS=ON`은 인증 헤더 직렬화 비교 실행 파일을 추가한다. kernel NAT 재현은 [호환성 표](compatibility-matrix.md)를 참고한다.

v1.0.0 공개 발행에는 `release/readiness.json`의 수동 실사용 증거가 추가로 필요하다. 현재 이 게이트는 pending이며 일반 push는 패키지 artifact만 생성한다.
