// xtouch_node.cpp
//
// Minimal ROS 2 bridge for the Behringer X-Touch Extender (MC mode, 8 faders).
//
// Protocol (from the Qt reference at reference/qt_midi_control/):
//   - Fader move : Pitch Bend  0xE0 | ch, LSB, MSB   (ch 0..7, 14-bit value)
//   - Fader touch: Note On/Off on notes 104..111     (note - 104 -> channel)
//
// Motor hold (MC mode quirk): the motorised faders return to whatever position
// the host last commanded. Without an OUT echo the motor reels every fader to
// 0 the moment the user lets go. We mirror the reference project's debounce
// strategy: 100 ms after the last incoming Pitch Bend on a channel, we send
// that same value back as Pitch Bend OUT, locking the motor in place.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <rtmidi/RtMidi.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int32.hpp>
#include <xtouch_midi/msg/x_touch_channel_state.hpp>
#include <xtouch_midi/msg/x_touch_state.hpp>

namespace {

constexpr std::size_t kNumChannels = 8;
constexpr uint8_t kFaderTouchNoteStart = 104;  // 104..111
constexpr uint8_t kFaderTouchNoteEnd = kFaderTouchNoteStart + kNumChannels - 1;
constexpr uint8_t kSelectNoteStart = 24;       // 24..31
constexpr uint8_t kSelectNoteEnd = kSelectNoteStart + kNumChannels - 1;
constexpr int32_t kFaderValueMax = 16383;      // 14-bit
constexpr auto kDebounceInterval = std::chrono::milliseconds(100);
constexpr auto kDebounceTick     = std::chrono::milliseconds(50);

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
    target_ids_ = load_target_ids();

    for (std::size_t i = 0; i < kNumChannels; ++i) {
      // ROS 2 topic name tokens cannot start with a digit, so we use "chN".
      const std::string suffix = "ch" + std::to_string(i);
      fader_pubs_[i] = create_publisher<std_msgs::msg::Int32>(
        "/xtouch/fader/" + suffix, 10);
      touch_pubs_[i] = create_publisher<std_msgs::msg::Bool>(
        "/xtouch/touch/" + suffix, 10);
    }
    state_pub_ = create_publisher<xtouch_midi::msg::XTouchState>(
      "/xtouch/state", 10);

    midi_in_ = std::make_unique<RtMidiIn>();
    midi_in_->ignoreTypes(true, true, true);  // SysEx, timing, active sensing
    open_matching_port(*midi_in_, "input");

    midi_out_ = std::make_unique<RtMidiOut>();
    open_matching_port(*midi_out_, "output");

    // Start input callback only after OUT is ready, so the first incoming
    // Pitch Bend can be echoed back without races.
    midi_in_->setCallback(&XTouchNode::midi_trampoline, this);

    debounce_tick_ = create_wall_timer(kDebounceTick,
      std::bind(&XTouchNode::tick_debounce, this));

    RCLCPP_INFO(get_logger(),
      "xtouch_node ready. Publishing per-channel topics and /xtouch/state; "
      "Select notes 24..31 map to enabled; motor hold via 100 ms debounce echo.");
  }

  ~XTouchNode() override
  {
    if (midi_in_) {
      midi_in_->cancelCallback();
      if (midi_in_->isPortOpen()) { midi_in_->closePort(); }
    }
    if (midi_out_ && midi_out_->isPortOpen()) {
      midi_out_->closePort();
    }
  }

private:
  template <typename Port>
  void open_matching_port(Port & port, const char * direction)
  {
    const unsigned int n = port.getPortCount();
    RCLCPP_INFO(get_logger(), "Scanning %u MIDI %s port(s)...", n, direction);

    for (unsigned int i = 0; i < n; ++i) {
      const std::string name = port.getPortName(i);
      RCLCPP_INFO(get_logger(), "  [%s %u] %s", direction, i, name.c_str());
      if (looks_like_xtouch(name)) {
        port.openPort(i);
        RCLCPP_INFO(get_logger(),
          "Connected MIDI %s port: '%s'", direction, name.c_str());
        return;
      }
    }

    throw std::runtime_error(
      std::string("No MIDI ") + direction
      + " port matching 'X-Touch'/'XTOUCH'/'Behringer' found. "
        "Is the device connected and powered on?");
  }

  static void midi_trampoline(double /*ts*/, std::vector<unsigned char> * msg,
    void * user_data)
  {
    if (msg == nullptr || user_data == nullptr) { return; }
    static_cast<XTouchNode *>(user_data)->on_midi(*msg);
  }

  std::array<uint32_t, kNumChannels> load_target_ids()
  {
    const auto values = declare_parameter<std::vector<int64_t>>(
      "target_ids", std::vector<int64_t>(kNumChannels, 0));
    if (values.size() != kNumChannels) {
      throw std::runtime_error(
        "Parameter 'target_ids' must contain exactly 8 integer values.");
    }

    std::array<uint32_t, kNumChannels> target_ids{};
    for (std::size_t i = 0; i < kNumChannels; ++i) {
      if (values[i] < 0 || values[i] > UINT32_MAX) {
        throw std::runtime_error(
          "Parameter 'target_ids' entries must be in uint32 range.");
      }
      target_ids[i] = static_cast<uint32_t>(values[i]);
    }
    return target_ids;
  }

  void publish_state_snapshot()
  {
    xtouch_midi::msg::XTouchState state_msg;
    state_msg.stamp = now();

    {
      std::lock_guard<std::mutex> lk(state_mutex_);
      for (std::size_t i = 0; i < kNumChannels; ++i) {
        auto & channel = state_msg.channels[i];
        channel.channel = static_cast<uint8_t>(i);
        channel.fader = current_fader_value_[i];
        channel.fader_changed = current_fader_value_[i] != published_fader_value_[i];
        channel.touch = touch_state_[i];
        channel.enabled = enabled_state_[i];
        channel.target_id = target_ids_[i];
      }
      published_fader_value_ = current_fader_value_;
    }

    state_pub_->publish(state_msg);
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

      // Arm the per-channel debounce so we echo this position back to the
      // device 100 ms after the user stops moving the fader.
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        current_fader_value_[channel] = value;
        debounce_deadline_[channel] =
          std::chrono::steady_clock::now() + kDebounceInterval;
      }

      publish_state_snapshot();

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
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        touch_state_[ch] = touched;
      }
      publish_state_snapshot();
      RCLCPP_INFO(get_logger(), "touch[%zu] = %s", ch, touched ? "down" : "up");
      return;
    }

    // --- Select button used as per-channel enabled state (Note On/Off, 24..31) ---
    if ((is_note_on || is_note_off)
        && d1 >= kSelectNoteStart && d1 <= kSelectNoteEnd) {
      const std::size_t ch = d1 - kSelectNoteStart;
      const bool enabled = is_note_on && d2 > 0;  // vel 0 == note off
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        enabled_state_[ch] = enabled;
      }
      publish_state_snapshot();
      RCLCPP_INFO(get_logger(), "enabled[%zu] = %s", ch, enabled ? "true" : "false");
      return;
    }
  }

  void tick_debounce()
  {
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t ch = 0; ch < kNumChannels; ++ch) {
      int32_t value = 0;
      bool fire = false;
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (debounce_deadline_[ch].has_value()
            && now >= *debounce_deadline_[ch]) {
          value = current_fader_value_[ch];
          debounce_deadline_[ch].reset();
          fire = true;
        }
      }
      if (fire) {
        send_fader_pitch_bend(static_cast<uint8_t>(ch), value);
      }
    }
  }

  void send_fader_pitch_bend(uint8_t ch, int32_t value)
  {
    value = std::clamp<int32_t>(value, 0, kFaderValueMax);
    std::vector<unsigned char> bytes = {
      static_cast<unsigned char>(0xE0 | (ch & 0x0F)),
      static_cast<unsigned char>(value & 0x7F),         // LSB
      static_cast<unsigned char>((value >> 7) & 0x7F),  // MSB
    };
    try {
      midi_out_->sendMessage(&bytes);
    } catch (const RtMidiError & e) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
        "MIDI out failed on ch %u: %s", ch, e.what());
    }
  }

  std::unique_ptr<RtMidiIn>  midi_in_;
  std::unique_ptr<RtMidiOut> midi_out_;

  std::array<rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr, kNumChannels>
    fader_pubs_;
  std::array<rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr, kNumChannels>
    touch_pubs_;
  rclcpp::Publisher<xtouch_midi::msg::XTouchState>::SharedPtr state_pub_;

  std::mutex state_mutex_;
  std::array<int32_t, kNumChannels> current_fader_value_{};
  std::array<int32_t, kNumChannels> published_fader_value_{};
  std::array<bool, kNumChannels> touch_state_{};
  std::array<bool, kNumChannels> enabled_state_{};
  std::array<uint32_t, kNumChannels> target_ids_{};
  std::array<std::optional<std::chrono::steady_clock::time_point>, kNumChannels>
    debounce_deadline_{};

  rclcpp::TimerBase::SharedPtr debounce_tick_;
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
