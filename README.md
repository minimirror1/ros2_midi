# ros2_midi — X-Touch Extender → ROS 2 브리지

**Behringer X-Touch Extender**의 8개 페이더(슬라이더) 위치, 터치 상태, 그리고 채널별
Rec/Solo/Mute/Select 버튼 토글 상태를 MIDI로 읽어 ROS 2 토픽으로 퍼블리시하는
UI 없는 최소 ROS 2 노드입니다. 버튼 토글은 동시에 디바이스 LED 로 미러링됩니다.

## 토픽

| Topic                      | Type                | 설명                                |
| -------------------------- | ------------------- | ----------------------------------- |
| `/xtouch/fader/ch0..ch7`   | `std_msgs/Int32`    | 14-bit 페이더 위치, `0..16383`       |
| `/xtouch/touch/ch0..ch7`   | `std_msgs/Bool`     | 페이더 터치 중일 때 `true`           |
| `/xtouch/state`            | `xtouch_midi/XTouchState` | 8채널 전체 상태 스냅샷         |

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

`target_id` 를 채널별로 지정하려면 `target_ids` 파라미터에 길이 8의 정수 배열을 넘기면 됩니다.

```bash
ros2 run xtouch_midi xtouch_node --ros-args \
  -p target_ids:="[101,102,103,104,105,106,107,108]"
```

노드는 기동 시 사용 가능한 MIDI 입력 포트 목록을 출력하고, 이름에
`X-Touch`, `XTOUCH`, `Behringer` 중 하나가 포함된 첫 번째 포트에 자동 연결합니다.

## 동작 확인 (다른 터미널)

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

ros2 topic list | grep xtouch          # 총 17개 토픽
ros2 topic echo /xtouch/fader/ch0      # 1번 슬라이더를 움직이면 0..16383 값 출력
ros2 topic echo /xtouch/touch/ch0      # 1번 슬라이더를 터치하면 true/false 출력
ros2 topic echo /xtouch/state          # 8채널 전체 상태 스냅샷
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

### 집계 상태 (`/xtouch/state`)

**타입**: `xtouch_midi/msg/XTouchState`

```text
builtin_interfaces/Time stamp
xtouch_midi/XTouchChannelState[8] channels

# XTouchChannelState
uint8 channel
int32 fader
bool fader_changed
bool touch
bool rec
bool solo
bool mute
bool select
uint32 target_id
```

- `channels[i].channel` 은 `0..7`.
- `channels[i].fader` 는 현재 14-bit raw 페이더 값입니다.
- `channels[i].fader_changed` 는 직전 `/xtouch/state` 발행 대비 값이 바뀌었을 때 `true` 입니다.
- `channels[i].touch` 는 페이더 터치 상태입니다.
- `channels[i].rec` / `solo` / `mute` / `select` 는 각 채널 strip 의 4종 버튼 토글 상태입니다.
  하드웨어는 누름/뗌 이벤트만 보내며, 노드가 누름마다 토글하고 LED 로 즉시 미러링합니다.
  노드 시작 시 모두 `false` 이며 LED 도 꺼진 상태에서 시작합니다.
- `channels[i].target_id` 는 `target_ids` ROS 파라미터에서 읽은 `uint32` 값입니다.

`ros2 topic echo /xtouch/state` 출력 예:

```yaml
stamp:
  sec: 1714464000
  nanosec: 123456789
channels:
- channel: 0
  fader: 8192
  fader_changed: true
  touch: false
  rec: false
  solo: false
  mute: true
  select: true
  target_id: 101
- channel: 1
  fader: 0
  fader_changed: false
  touch: false
  rec: false
  solo: false
  mute: false
  select: false
  target_id: 102
```

### 채널 strip 버튼 (Rec / Solo / Mute / Select) 와 LED

원본 MIDI 매핑 (MIDI 채널 0):

| 버튼   | 노트 범위 | kind 인덱스 |
| ------ | --------- | ----------- |
| Rec    | `0..7`    | 0           |
| Solo   | `8..15`   | 1           |
| Mute   | `16..23`  | 2           |
| Select | `24..31`  | 3           |

- 입력: 누름 = `Note On` (vel > 0), 뗌 = `Note Off` 또는 `Note On` vel=0.
  노드는 누름 이벤트에만 반응하여 해당 버튼의 토글 상태를 반전시킵니다.
- 출력: 토글이 바뀔 때마다 즉시 같은 노트 번호로 `Note On` 송신.
  vel `127` = LED ON, vel `0` = LED OFF.
- 시작 시 32개 LED 모두 OFF 로 송신해 디바이스 LED 와 노드 내부 상태를 동기화합니다.

### Encoder rotate / push (내부 사용)

엔코더 회전과 누름은 노드 내부 상태로만 유지하며, 별도 토픽이나 메시지 필드로
노출하지 않습니다. 외부 효과는 LED Ring 표시뿐입니다.

| 입력   | MIDI                          | 동작                                           |
| ------ | ----------------------------- | ---------------------------------------------- |
| Rotate | CC `80..87` (MIDI 채널 0)     | value `1..63` = CW(+1), `65..127` = CCW(-1)   |
| Push   | Note `32..39` (MIDI 채널 0)   | 누름마다 내부 토글 (외부 효과 없음, 추후 활용) |
| LED Ring (출력) | CC `48..55`           | value = `(2 << 4) \| position`, mode 2 = wrap |

- 회전 카운터는 채널별로 `0..11` 범위 saturate. CW 로 12 클릭 이상 돌려도 11 에서 멈춥니다.
- LED Ring 은 카운터값이 변할 때마다 즉시 갱신 (왼쪽부터 N 개 점등).
- 노드 시작 시 8개 ring 모두 position 0 (모두 소등) 으로 초기화합니다.

### 퍼블리시 규칙과 QoS

- **이벤트 기반**: 주기적 발행이 아니라, 장치에서 MIDI 메시지가 도착한 순간에만 퍼블리시합니다.
  페이더를 움직이지 않으면 토픽에 아무 값도 나오지 않습니다. `/xtouch/state` 역시
  페이더, 터치, 또는 Rec/Solo/Mute/Select 버튼 누름이 발생한 순간에만 발행됩니다.
- **QoS**: 기본값 `RMW_QOS_POLICY_*_DEFAULT` (Reliable, Volatile, depth = 10).
  컨트롤 입력용이므로 과거 값 재생(Transient Local)은 하지 않습니다. 구독자가 늦게 붙으면
  다음 페이더 움직임부터 수신합니다.
- **순서 보장**: 동일 토픽 내에서 발행 순서대로 전달됩니다 (DDS 기본 보장).

## 참고

MIDI 프로토콜 세부사항은 `reference/qt_midi_control/`(Windows Qt + RtMidi 프로젝트)에서
추출했습니다. X-Touch Extender는 기본 XCtl 모드로 사용하며 MCU/HUI 핸드셰이크는 전송하지
않습니다.
