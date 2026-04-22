// xtouch_node.cpp
//
// Minimal ROS 2 bridge for the Behringer X-Touch Extender (XCtl mode, 8 faders).
//
// Protocol (from the Qt reference at reference/qt_midi_control/):
//   - Fader move : Pitch Bend  0xE0 | ch, LSB, MSB   (ch 0..7, 14-bit value)
//   - Fader touch: Note On/Off on notes 104..111     (note - 104 -> channel)
// No MCU/HUI handshake is required; we just listen on the matched port.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <rtmidi/RtMidi.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>

namespace {

constexpr std::size_t kNumChannels = 8;
constexpr uint8_t kFaderTouchNoteStart = 104;  // 104..111
constexpr uint8_t kFaderTouchNoteEnd = kFaderTouchNoteStart + kNumChannels - 1;

std::string to_upper(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

bool looks_like_xtouch(const std::string & port_name)
{
  const std::string up = to_upper(port_name);
  return up.find("X-TOUCH") != std::string::npos
      || up.find("XTOUCH")  != std::string::npos
      || up.find("BEHRINGER") != std::string::npos;
}

}  // namespace


class XTouchNode : public rclcpp::Node
{
public:
  XTouchNode()
  : rclcpp::Node("xtouch_node")
  {
    for (std::size_t i = 0; i < kNumChannels; ++i) {
      // ROS 2 topic name tokens cannot start with a digit, so we use "chN".
      const std::string suffix = "ch" + std::to_string(i);
      fader_pubs_[i] = create_publisher<std_msgs::msg::Int32>(
        "/xtouch/fader/" + suffix, 10);
      touch_pubs_[i] = create_publisher<std_msgs::msg::Bool>(
        "/xtouch/touch/" + suffix, 10);
    }

    midi_in_ = std::make_unique<RtMidiIn>();
    midi_in_->ignoreTypes(true, true, true);  // SysEx, timing, active sensing

    open_matching_port();

    midi_in_->setCallback(&XTouchNode::midi_trampoline, this);

    RCLCPP_INFO(get_logger(),
      "xtouch_node ready. Publishing 8 faders + 8 touches on /xtouch/...");
  }

  ~XTouchNode() override
  {
    if (midi_in_) {
      midi_in_->cancelCallback();
      if (midi_in_->isPortOpen()) {
        midi_in_->closePort();
      }
    }
  }

private:
  void open_matching_port()
  {
    const unsigned int n = midi_in_->getPortCount();
    RCLCPP_INFO(get_logger(), "Scanning %u MIDI input port(s)...", n);

    for (unsigned int i = 0; i < n; ++i) {
      const std::string name = midi_in_->getPortName(i);
      RCLCPP_INFO(get_logger(), "  [%u] %s", i, name.c_str());
      if (looks_like_xtouch(name)) {
        midi_in_->openPort(i);
        RCLCPP_INFO(get_logger(), "Connected to MIDI port: '%s'", name.c_str());
        return;
      }
    }

    throw std::runtime_error(
      "No MIDI input port matching 'X-Touch'/'XTOUCH'/'Behringer' found. "
      "Is the device connected and powered on?");
  }

  static void midi_trampoline(double /*ts*/, std::vector<unsigned char> * msg,
    void * user_data)
  {
    if (msg == nullptr || user_data == nullptr) { return; }
    static_cast<XTouchNode *>(user_data)->on_midi(*msg);
  }

  void on_midi(const std::vector<unsigned char> & bytes)
  {
    if (bytes.size() < 3) { return; }

    const uint8_t status  = bytes[0] & 0xF0;
    const uint8_t channel = bytes[0] & 0x0F;
    const uint8_t d1 = bytes[1];
    const uint8_t d2 = bytes[2];

    // --- Fader (Pitch Bend, 14-bit) ---
    if (status == 0xE0 && channel < kNumChannels) {
      const int32_t value = static_cast<int32_t>((d2 << 7) | d1);
      std_msgs::msg::Int32 m;
      m.data = value;
      fader_pubs_[channel]->publish(m);
      RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 500,
        "fader[%u] = %d", channel, value);
      return;
    }

    // --- Fader touch (Note On/Off, notes 104..111) ---
    const bool is_note_on  = (status == 0x90);
    const bool is_note_off = (status == 0x80);
    if ((is_note_on || is_note_off)
        && d1 >= kFaderTouchNoteStart && d1 <= kFaderTouchNoteEnd) {
      const std::size_t ch = d1 - kFaderTouchNoteStart;
      const bool touched = is_note_on && d2 > 0;  // vel 0 == note off
      std_msgs::msg::Bool m;
      m.data = touched;
      touch_pubs_[ch]->publish(m);
      RCLCPP_INFO(get_logger(), "touch[%zu] = %s", ch, touched ? "down" : "up");
      return;
    }
  }

  std::unique_ptr<RtMidiIn> midi_in_;
  std::array<rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr, kNumChannels>
    fader_pubs_;
  std::array<rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr, kNumChannels>
    touch_pubs_;
};


int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    auto node = std::make_shared<XTouchNode>();
    rclcpp::spin(node);
  } catch (const RtMidiError & e) {
    RCLCPP_FATAL(rclcpp::get_logger("xtouch_node"),
      "RtMidi error: %s", e.what());
    rclcpp::shutdown();
    return 2;
  } catch (const std::exception & e) {
    RCLCPP_FATAL(rclcpp::get_logger("xtouch_node"), "%s", e.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}
