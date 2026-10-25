<img src="assets/branding/lanlink.png" width="80" alt="LanLink">

# LanLink

여러 Windows PC를 가상 IPv4 네트워크로 연결하는 프로그램이다. 게임이나 일반 애플리케이션에서 상대 기기의 LanLink IP로 접속한다. 예를 들어 게임 서버의 가상 IP가 `10.77.0.2`라면 `10.77.0.2:8211`처럼 사용한다.

Windows 서비스가 연결과 가상 어댑터를 유지하고, 데스크톱 UI에서 네트워크 생성·가입·기기 승인·가상 IP 복사를 처리한다. QUIC UDP를 사용하며 연결할 수 없으면 TLS TCP로 전환한다. 가상 네트워크 패킷은 클라이언트 사이에서 암호화하고 릴레이는 암호문을 전달한다.

## 설치

[Releases](https://github.com/Koreapanda4444/LanLink/releases/latest)에서 Windows용 `LanLink-Setup-0.2.2-windows-x64.exe`를 실행한다. 릴레이 주소와 관리자가 전달한 인증 토큰 파일을 입력하면 서비스, UI, 런타임 라이브러리와 Wintun을 함께 설치한다. ZIP은 PowerShell로 수동 설치할 때 사용한다.

릴레이는 특정 클라우드에 종속되지 않는다. Oracle Cloud, 다른 VPS, 자체 서버에서 운영할 수 있다. 배포 파일은 Linux x64용이며 glibc 2.34 이상과 systemd를 기준으로 한다. 같은 패키지를 Oracle Linux 9, Ubuntu 22.04, Debian 12에서 검증한다. 서버 설치와 방화벽 설정은 [설치 안내](release/INSTALL.md)를 참고한다.

설치를 시작할 때 한국어 또는 영어를 선택할 수 있다. 이 선택은 설치 프로그램의 표시 언어에 적용되며, 데스크톱 UI는 영어로 제공한다.

## 구성

- `lanlink-relay`: 서버의 패킷 중계와 네트워크 관리
- `lanlink-service`: Windows 자동 시작 서비스와 Wintun 가상 어댑터
- `lanlink-ui`: 네트워크·기기·접속 상태를 관리하는 데스크톱 화면
- `lanlink-ui-cli`: 명령줄 관리 도구

## 소스 빌드

CMake 3.24 이상, C++20 컴파일러, 저장소 `vcpkg.json`의 baseline과 일치하는 vcpkg가 필요하다.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

Windows에서는 DirectX 11/Win32 UI와 서비스를 함께 빌드한다. Linux 릴레이 패키지는 `VCPKG_ROOT`를 지정한 뒤 `bash deploy/linux/build-relay.sh`로 만든다. 직접 빌드하는 서버의 라이브러리·glibc 요구 사항은 해당 빌드 환경을 따른다.
