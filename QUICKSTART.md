# Quick Start

Get the `xtouch_midi` node up and streaming fader data in under a minute,
assuming dependencies are already installed (see `deps/install_deps.sh`) and
the workspace has been built (`colcon build --symlink-install`).

## 1. Plug in the device

Connect the Behringer **X-Touch Extender** via USB and power it on.
Verify the OS sees it:

```bash
amidi -l    # expect a row containing "X-Touch-Ext"
```

## 2. Source the environment

```bash
source /opt/ros/humble/setup.bash
source /home/hexapod/Desktop/ros2_midi/install/setup.bash
```

## 3. Run the node

```bash
ros2 run xtouch_midi xtouch_node
```

Expected startup log:

```
[INFO] xtouch_node: Scanning N MIDI input port(s)...
[INFO] xtouch_node:   [i] X-Touch-Ext:X-Touch-Ext X-TOUCH_INT 20:0
[INFO] xtouch_node: Connected to MIDI port: 'X-Touch-Ext:...'
[INFO] xtouch_node: xtouch_node ready. Publishing 8 faders + 8 touches on /xtouch/...
```

## 4. Observe the topics (in another terminal)

```bash
source /opt/ros/humble/setup.bash
source /home/hexapod/Desktop/ros2_midi/install/setup.bash

ros2 topic list | grep xtouch      # 16 topics: fader/ch0..ch7, touch/ch0..ch7
ros2 topic echo /xtouch/fader/ch0  # move slider 1 -> 0..16383
ros2 topic echo /xtouch/touch/ch0  # touch slider 1 -> true/false
ros2 topic hz   /xtouch/fader/ch0  # publish rate while sliding
```

## 5. Stop the node

`Ctrl+C` in the terminal running `ros2 run`.

## Topic reference

| Topic                        | Type             | Payload                          |
| ---------------------------- | ---------------- | -------------------------------- |
| `/xtouch/fader/ch0` … `ch7`  | `std_msgs/Int32` | 14-bit fader value, `0..16383`   |
| `/xtouch/touch/ch0` … `ch7`  | `std_msgs/Bool`  | `true` while finger on the strip |

## Troubleshooting

- **`No MIDI input port matching ...`** — device not connected or not
  powered. Run `amidi -l` and confirm an `X-Touch-Ext` row appears.
- **`Permission denied` on the MIDI device** — add your user to the `audio`
  group: `sudo usermod -aG audio $USER`, then log out and back in.
- **Topics not showing up** — ROS 2 DDS discovery can take ~1 s. Wait a
  moment and re-run `ros2 topic list`.
