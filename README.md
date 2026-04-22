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

---

## 메시지 형식

### 페이더 위치 (`/xtouch/fader/chN`)

**타입**: [`std_msgs/msg/Int32`](https://docs.ros.org/en/humble/p/std_msgs/msg/Int32.html)

```
# std_msgs/msg/Int32.msg
int32 data
```

- `data` 에 14-bit raw 값이 들어갑니다. 범위 `[0, 16383]`.
  - `0` = 슬라이더 최하단, `16383` = 최상단. 중앙 근처 값은 약 `8192`.
  - 원본 MIDI Pitch Bend 메시지(`0xE0|ch, LSB, MSB`)를 `(MSB << 7) | LSB` 로 복원한 값 그대로입니다.
- 정규화가 필요한 구독자는 `data / 16383.0` 으로 `[0.0, 1.0]` float 으로 변환하면 됩니다.

`ros2 topic echo /xtouch/fader/ch0` 출력 예:

```yaml
---
data: 8192
---
data: 10421
---
data: 16383
---
```

### 페이더 터치 (`/xtouch/touch/chN`)

**타입**: [`std_msgs/msg/Bool`](https://docs.ros.org/en/humble/p/std_msgs/msg/Bool.html)

```
# std_msgs/msg/Bool.msg
bool data
```

- `data: true` — 손가락이 페이더 스트립에 닿아 있는 동안.
- `data: false` — 손가락을 뗀 순간.
- 원본은 Note On/Off (노트 `104..111`). `note - 104` 가 채널 번호와 일치합니다.

`ros2 topic echo /xtouch/touch/ch3` 출력 예:

```yaml
---
data: true
---
data: false
---
```

### 퍼블리시 규칙과 QoS

- **이벤트 기반**: 주기적 발행이 아니라, 장치에서 MIDI 메시지가 도착한 순간에만 퍼블리시합니다.
  페이더를 움직이지 않으면 토픽에 아무 값도 나오지 않습니다.
- **QoS**: 기본값 `RMW_QOS_POLICY_*_DEFAULT` (Reliable, Volatile, depth = 10).
  컨트롤 입력용이므로 과거 값 재생(Transient Local)은 하지 않습니다. 구독자가 늦게 붙으면
  다음 페이더 움직임부터 수신합니다.
- **순서 보장**: 동일 토픽 내에서 발행 순서대로 전달됩니다 (DDS 기본 보장).

## 참고

MIDI 프로토콜 세부사항은 `reference/qt_midi_control/`(Windows Qt + RtMidi 프로젝트)에서
추출했습니다. X-Touch Extender는 기본 XCtl 모드로 사용하며 MCU/HUI 핸드셰이크는 전송하지
않습니다.
