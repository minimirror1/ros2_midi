# ros2_midi — X-Touch Extender → ROS 2 브리지

**Behringer X-Touch Extender**의 8개 페이더(슬라이더) 위치와 터치 상태를 MIDI로 읽어
ROS 2 토픽으로 퍼블리시하는 UI 없는 최소 ROS 2 노드입니다.

## 토픽

| Topic                      | Type                | 설명                                |
| -------------------------- | ------------------- | ----------------------------------- |
| `/xtouch/fader/ch0..ch7`   | `std_msgs/Int32`    | 14-bit 페이더 위치, `0..16383`       |
| `/xtouch/touch/ch0..ch7`   | `std_msgs/Bool`     | 페이더 터치 중일 때 `true`           |

## 사전 요구사항

- Ubuntu 22.04
- Behringer X-Touch Extender가 USB로 연결되고 전원이 켜져 있을 것
- ROS 2 + librtmidi 설치를 위한 sudo 권한

## 의존성 설치 (최초 1회)

```bash
bash deps/install_deps.sh
```

ROS 2 Humble apt 저장소를 등록하고, `deps/apt_packages.txt`에 나열된 모든 패키지
(ros-humble-ros-base, colcon, librtmidi-dev, 빌드 도구 등)를 설치합니다.

## 빌드

```bash
source /opt/ros/humble/setup.bash
colcon build --symlink-install
source install/setup.bash
```

## 실행

```bash
ros2 run xtouch_midi xtouch_node
```

노드는 기동 시 사용 가능한 MIDI 입력 포트 목록을 출력하고, 이름에
`X-Touch`, `XTOUCH`, `Behringer` 중 하나가 포함된 첫 번째 포트에 자동 연결합니다.

## 동작 확인 (다른 터미널)

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

ros2 topic list | grep xtouch          # 총 16개 토픽
ros2 topic echo /xtouch/fader/ch0      # 1번 슬라이더를 움직이면 0..16383 값 출력
ros2 topic echo /xtouch/touch/ch0      # 1번 슬라이더를 터치하면 true/false 출력
```

## 참고

MIDI 프로토콜 세부사항은 `reference/qt_midi_control/`(Windows Qt + RtMidi 프로젝트)에서
추출했습니다. X-Touch Extender는 기본 XCtl 모드로 사용하며 MCU/HUI 핸드셰이크는 전송하지
않습니다.
