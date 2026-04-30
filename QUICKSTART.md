# 퀵 스타트

의존성이 설치되어 있고(`deps/install_deps.sh` 참고) 워크스페이스가 빌드되어 있다면
(`colcon build --symlink-install`), 아래 순서대로 1분 안에 `xtouch_midi` 노드를
기동하고 페이더 데이터를 스트리밍할 수 있습니다.

## 1. 장치 연결

Behringer **X-Touch Extender**를 USB로 연결하고 전원을 켭니다. OS에서 인식되는지 확인:

```bash
amidi -l    # "X-Touch-Ext" 가 포함된 행이 보여야 함
```

## 2. 환경 소싱

```bash
source /opt/ros/humble/setup.bash
source /home/hexapod/Desktop/ros2_midi/install/setup.bash
```

## 3. 노드 실행

```bash
ros2 run xtouch_midi xtouch_node
```

`target_id` 테스트가 필요하면 실행 시 파라미터를 넘길 수 있습니다.

```bash
ros2 run xtouch_midi xtouch_node --ros-args \
  -p target_ids:="[101,102,103,104,105,106,107,108]"
```

정상 기동 시 로그 예시:

```
[INFO] xtouch_node: Scanning N MIDI input port(s)...
[INFO] xtouch_node:   [i] X-Touch-Ext:X-Touch-Ext X-TOUCH_INT 20:0
[INFO] xtouch_node: Connected MIDI input port: 'X-Touch-Ext:...'
[INFO] xtouch_node: Connected MIDI output port: 'X-Touch-Ext:...'
[INFO] xtouch_node: xtouch_node ready. Publishing per-channel topics and /xtouch/state; Select notes 24..31 map to enabled; motor hold via 100 ms debounce echo.
```

## 4. 토픽 확인 (다른 터미널)

```bash
source /opt/ros/humble/setup.bash
source /home/hexapod/Desktop/ros2_midi/install/setup.bash

ros2 topic list | grep xtouch      # 17개 토픽: fader/ch0..ch7, touch/ch0..ch7, state
ros2 topic echo /xtouch/fader/ch0  # 1번 슬라이더 움직임 -> 0..16383
ros2 topic echo /xtouch/touch/ch0  # 1번 슬라이더 터치  -> true/false
ros2 topic echo /xtouch/state      # 전체 8채널 상태 스냅샷
ros2 topic hz   /xtouch/fader/ch0  # 슬라이딩 중 퍼블리시 주기 측정
```

## 5. 노드 종료

`ros2 run` 이 실행 중인 터미널에서 `Ctrl+C`.

## 토픽 레퍼런스

| Topic                        | Type                      | Payload                              |
| ---------------------------- | ------------------------- | ------------------------------------ |
| `/xtouch/fader/ch0` … `ch7`  | `std_msgs/Int32`          | 14-bit 페이더 값, `0..16383`          |
| `/xtouch/touch/ch0` … `ch7`  | `std_msgs/Bool`           | 페이더에 손가락이 닿아 있는 동안 `true` |
| `/xtouch/state`              | `xtouch_midi/XTouchState` | 8채널 집계 상태 스냅샷                |

### 메시지 페이로드 예시

`std_msgs/msg/Int32` 와 `std_msgs/msg/Bool` 은 단일 필드 `data` 하나만 가집니다.
`ros2 topic echo` 출력은 YAML 형태로 한 메시지씩 표시됩니다.

```yaml
# /xtouch/fader/ch0  (페이더 움직임)
---
data: 8192      # 슬라이더 중앙 근처
---
data: 16383     # 최상단
---

# /xtouch/touch/ch0  (페이더 터치/릴리즈)
---
data: true      # 손가락 접촉
---
data: false     # 손 떼는 순간
---

# /xtouch/state  (예시 일부)
stamp:
  sec: 1714464000
  nanosec: 123456789
channels:
- channel: 0
  fader: 8192
  fader_changed: true
  touch: false
  enabled: true
  target_id: 101
```

이벤트 기반 발행(주기 X)이므로, 장치 입력이 없으면 토픽에도 아무 값도 나오지 않습니다.
`enabled` 는 각 채널의 `Select` 버튼(Note `24..31`) 상태입니다.
상세 프로토콜 매핑과 정규화 팁은 `README.md` 의 "메시지 형식" 참조.

## 트러블슈팅

- **`No MIDI input port matching ...`** — 장치가 연결되지 않았거나 전원이 꺼져 있습니다.
  `amidi -l` 을 실행해 `X-Touch-Ext` 행이 나오는지 확인하세요.
- **MIDI 장치 `Permission denied`** — 사용자를 `audio` 그룹에 추가:
  `sudo usermod -aG audio $USER` 후 로그아웃 → 재로그인.
- **토픽이 보이지 않음** — ROS 2 DDS 디스커버리에 약 1초가 걸릴 수 있습니다. 잠시 기다렸다
  `ros2 topic list` 를 다시 실행하세요.
