#!/usr/bin/env bash
# ============================================================================
# install_deps.sh — Provision Ubuntu 22.04 for the ros2_midi workspace.
#
# What this does:
#   1. Ensure UTF-8 locale (ROS 2 requirement).
#   2. Register the ROS 2 Humble apt repository + signing key.
#   3. apt install every package listed in deps/apt_packages.txt.
#
# Safe to re-run; each step is idempotent.
#
# Run once as a user with sudo:
#     bash deps/install_deps.sh
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_LIST="${SCRIPT_DIR}/apt_packages.txt"

if [[ ! -f "${PKG_LIST}" ]]; then
    echo "ERROR: package list not found: ${PKG_LIST}" >&2
    exit 1
fi

echo "==> [1/4] Ensuring UTF-8 locale"
sudo apt-get update
sudo apt-get install -y locales
sudo locale-gen en_US en_US.UTF-8
sudo update-locale LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 || true

echo "==> [2/4] Enabling Ubuntu Universe repository"
sudo apt-get install -y software-properties-common
sudo add-apt-repository -y universe

echo "==> [3/4] Registering ROS 2 apt repository (GPG key + sources list)"
sudo apt-get install -y curl
sudo install -d -m 0755 /etc/apt/keyrings
if [[ ! -f /usr/share/keyrings/ros-archive-keyring.gpg ]]; then
    sudo curl -sSL \
        https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
        -o /usr/share/keyrings/ros-archive-keyring.gpg
fi
ROS_APT_SOURCE=/etc/apt/sources.list.d/ros2.list
if [[ ! -f "${ROS_APT_SOURCE}" ]]; then
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu $(. /etc/os-release && echo "${UBUNTU_CODENAME}") main" \
        | sudo tee "${ROS_APT_SOURCE}" > /dev/null
fi

echo "==> [4/4] Installing packages from ${PKG_LIST}"
sudo apt-get update
# shellcheck disable=SC2046
sudo apt-get install -y $(grep -vE '^\s*(#|$)' "${PKG_LIST}")

echo
echo "Done. Next steps:"
echo "  source /opt/ros/humble/setup.bash"
echo "  cd $(dirname "${SCRIPT_DIR}") && colcon build --symlink-install"
echo "  source install/setup.bash"
echo "  ros2 run xtouch_midi xtouch_node"
