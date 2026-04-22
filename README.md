# ros2_midi — X-Touch Extender → ROS 2 bridge

Minimal headless ROS 2 node that reads the 8 faders (and their touch state) of
a **Behringer X-Touch Extender** over MIDI and publishes them to ROS 2 topics.

## Topics

| Topic                      | Type                | Meaning                            |
| -------------------------- | ------------------- | ---------------------------------- |
| `/xtouch/fader/ch0..ch7`   | `std_msgs/Int32`    | 14-bit fader position, `0..16383`  |
| `/xtouch/touch/ch0..ch7`   | `std_msgs/Bool`     | `true` while fader is touched      |

## Prerequisites

- Ubuntu 22.04
- Behringer X-Touch Extender connected via USB, powered on
- sudo privileges for installing ROS 2 + librtmidi

## Install dependencies (one-time)

```bash
bash deps/install_deps.sh
```

This registers the ROS 2 Humble apt repository and installs every package
listed in `deps/apt_packages.txt` (ros-humble-ros-base, colcon, librtmidi-dev,
build tools).

## Build

```bash
source /opt/ros/humble/setup.bash
colcon build --symlink-install
source install/setup.bash
```

## Run

```bash
ros2 run xtouch_midi xtouch_node
```

On startup the node lists the available MIDI input ports and connects to the
first one whose name contains `X-Touch`, `XTOUCH`, or `Behringer`.

## Verify (in a second terminal)

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

ros2 topic list | grep xtouch          # 16 topics expected
ros2 topic echo /xtouch/fader/ch0      # move slider 1 -> values 0..16383
ros2 topic echo /xtouch/touch/ch0      # touch slider 1 -> true/false
```

## Reference

MIDI protocol details were extracted from
`reference/qt_midi_control/` (Windows Qt + RtMidi project). The Extender is
used in its default XCtl mode — no MCU/HUI handshake is sent.
