# LanLink

LanLink는 중앙 릴레이를 통해 여러 Windows PC를 하나의 가상 IPv4 네트워크처럼 연결하는 프로젝트다. 특정 게임에 종속되지 않으며, 게임에서는 상대방의 LanLink 가상 IP로 접속한다.

예를 들어 서버를 연 PC가 `10.77.0.2`를 받았다면 Palworld에서는 `10.77.0.2:8211`처럼 접속한다. Oracle VM의 공인 IP는 클라이언트가 릴레이에 연결할 때만 사용하며, 게임 주소로 사용하지 않는다.

## 구성 요소

- `lanlink-relay`: Linux 서버에서 실행할 중앙 패킷 릴레이
- `lanlink-service`: Windows 부팅 시 자동 시작할 백그라운드 클라이언트
- `lanlink-ui`: Dear ImGui Win32/DirectX 11 관리 화면
- `lanlink-core`: 세 실행 파일이 공유하는 C++ 코드


## 빌드

필요 도구:

- CMake 3.24 이상
- C++20 컴파일러

```sh
cmake -S . -B build
cmake --build build
```

Linux에서는 `lanlink-relay`만 빌드한다. Windows에서는 릴레이, 서비스, UI 실행 파일을 모두 빌드한다.