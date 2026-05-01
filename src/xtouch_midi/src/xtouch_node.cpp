// xtouch_node.cpp
//
// Minimal ROS 2 bridge for the Behringer X-Touch Extender (MC mode, 8 faders).
//
// Protocol (from the Qt reference at reference/qt_midi_control/):
//   - Fader move : Pitch Bend  0xE0 | ch, LSB, MSB   (ch 0..7, 14-bit value)
//   - Fader touch: Note On/Off on notes 104..111     (note - 104 -> channel)
//   - Channel-strip buttons: Note On/Off on notes 0..31, MIDI channel 0
//       Rec    = 0..7,  Solo = 8..15,  Mute = 16..23,  Select = 24..31
//     Hardware emits press/release only; this node maintains a per-button
//     toggle and mirrors it to the device LED via Note On (vel 127=ON, 0=OFF).
//   - Encoder push : Note On/Off on notes 32..39 (toggled internally only;
//                    no external observable effect, kept for future use).
//   - Encoder rotate (MC mode): CC 80..87, value 1..63 = CW (delta +1),
//                    65..127 = CCW (delta -1). Counter is per-channel,
//                    saturated to 0..11, and used internally only.
//   - Encoder LED ring: CC 48..55, value = (mode << 4) | position.
//                    Mode 2 = Wrap fill from left; position 0..11 = LED count.
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
// Channel-strip buttons (4 kinds × 8 channels = notes 0..31), MCU mapping.
// kind index: 0=Rec, 1=Solo, 2=Mute, 3=Select.
constexpr std::size_t kNumButtonKinds = 4;
constexpr uint8_t kButtonNoteStart = 0;
constexpr uint8_t kButtonNoteEnd = kButtonNoteStart
  + kNumButtonKinds * kNumChannels - 1;  // 31
constexpr uint8_t kLedVelocityOn  = 127;
constexpr uint8_t kLedVelocityOff = 0;
constexpr uint8_t kLedMidiChannel = 0;
// Encoder (MC mode): push as Note 32..39, rotate as CC 80..87 (relative),
// LED ring as CC 48..55 with value = (mode<<4)|position.
constexpr uint8_t kEncoderPushNoteStart = 32;  // 32..39
constexpr uint8_t kEncoderPushNoteEnd   =
  kEncoderPushNoteStart + kNumChannels - 1;
constexpr uint8_t kEncoderRotateCcStart = 80;  // 80..87
constexpr uint8_t kEncoderRotateCcEnd   =
  kEncoderRotateCcStart + kNumChannels - 1;
constexpr uint8_t kEncoderLedRingCcStart = 48;  // 48..55
constexpr uint8_t kEncoderRingMaxPosition = 11;
constexpr uint8_t kEncoderRingFillMode    = 2;  // Wrap: fill from left
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

    // Initialize all channel-strip button LEDs to OFF so the device matches
    // our internal toggle state (all false) right after startup.
    for (uint8_t note = kButtonNoteStart; note <= kButtonNoteEnd; ++note) {
      send_button_led(note, false);
    }

    // Initialize all encoder LED rings to position 0 (no LED lit) so the ring
    // matches encoder_count_ which starts at 0.
    for (std::size_t ch = 0; ch < kNumChannels; ++ch) {
      send_encoder_led_ring(ch, 0);
    }

    // Start input callback only after OUT is ready, so the first incoming
    // Pitch Bend can be echoed back without races.
    midi_in_->setCallback(&XTouchNode::midi_trampoline, this);

    debounce_tick_ = create_wall_timer(kDebounceTick,
      std::bind(&XTouchNode::tick_debounce, this));

    RCLCPP_INFO(get_logger(),
      "xtouch_node ready. Publishing per-channel topics and /xtouch/state; "
      "Rec/Solo/Mute/Select (notes 0..31) toggle on press, mirrored on LEDs; "
      "encoder rotate (CC 80..87) drives internal 0..11 counter and LED ring "
      "(CC 48..55, mode 2); motor hold via 100 ms debounce echo.");
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
        channel.rec    = button_state_[0][i];
        channel.solo   = button_state_[1][i];
        channel.mute   = button_state_[2][i];
        channel.select = button_state_[3][i];
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

    // --- Channel-strip buttons (Note 0..31): Rec/Solo/Mute/Select ---
    // The device only emits press/release events; we maintain toggle state in
    // software and mirror it on the LED. Release events are ignored so a press
    // produces exactly one toggle.
    if (is_note_on && d2 > 0
        && d1 >= kButtonNoteStart && d1 <= kButtonNoteEnd) {
      const std::size_t kind = (d1 - kButtonNoteStart) / kNumChannels;
      const std::size_t ch   = (d1 - kButtonNoteStart) % kNumChannels;
      bool new_state;
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        button_state_[kind][ch] = !button_state_[kind][ch];
        new_state = button_state_[kind][ch];
      }
      send_button_led(d1, new_state);
      publish_state_snapshot();
      static constexpr const char * kKindName[kNumButtonKinds] = {
        "rec", "solo", "mute", "select"
      };
      RCLCPP_INFO(get_logger(), "%s[%zu] = %s",
        kKindName[kind], ch, new_state ? "on" : "off");
      return;
    }
    if ((is_note_on || is_note_off)
        && d1 >= kButtonNoteStart && d1 <= kButtonNoteEnd) {
      // Release / vel=0 — explicitly ignored; toggling already happened on press.
      return;
    }

    // --- Encoder push (Note 32..39) ---
    // Internal toggle only; no LED, no message field, no topic. Reserved for
    // future use.
    if (is_note_on && d2 > 0
        && d1 >= kEncoderPushNoteStart && d1 <= kEncoderPushNoteEnd) {
      const std::size_t ch = d1 - kEncoderPushNoteStart;
      bool new_state;
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        encoder_push_state_[ch] = !encoder_push_state_[ch];
        new_state = encoder_push_state_[ch];
      }
      RCLCPP_INFO(get_logger(), "encoder_push[%zu] = %s",
        ch, new_state ? "on" : "off");
      return;
    }
    if ((is_note_on || is_note_off)
        && d1 >= kEncoderPushNoteStart && d1 <= kEncoderPushNoteEnd) {
      return;  // ignore release / vel=0
    }

    // --- Encoder rotate (CC 80..87, MC mode relative) ---
    // value 1..63 = CW (+1), 65..127 = CCW (-1). Counter saturates at 0..11
    // and is mirrored on the LED ring (CC 48..55, mode 2 = wrap fill).
    const bool is_cc = (status == 0xB0);
    if (is_cc
        && d1 >= kEncoderRotateCcStart && d1 <= kEncoderRotateCcEnd) {
      int delta = 0;
      if (d2 >= 1 && d2 <= 63) {
        delta = +1;
      } else if (d2 >= 65 && d2 <= 127) {
        delta = -1;
      }
      if (delta == 0) { return; }

      const std::size_t ch = d1 - kEncoderRotateCcStart;
      uint8_t new_count;
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        const int next = static_cast<int>(encoder_count_[ch]) + delta;
        new_count = static_cast<uint8_t>(
          std::clamp(next, 0, static_cast<int>(kEncoderRingMaxPosition)));
        encoder_count_[ch] = new_count;
      }
      send_encoder_led_ring(ch, new_count);
      RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 200,
        "encoder[%zu] = %u", ch, new_count);
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

  void send_button_led(uint8_t note, bool on)
  {
    std::vector<unsigned char> bytes = {
      static_cast<unsigned char>(0x90 | (kLedMidiChannel & 0x0F)),
      note,
      static_cast<unsigned char>(on ? kLedVelocityOn : kLedVelocityOff),
    };
    try {
      midi_out_->sendMessage(&bytes);
    } catch (const RtMidiError & e) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
        "MIDI out (LED) failed on note %u: %s", note, e.what());
    }
  }

  void send_encoder_led_ring(std::size_t ch, uint8_t position)
  {
    position = std::min(position, kEncoderRingMaxPosition);
    const uint8_t value =
      static_cast<uint8_t>((kEncoderRingFillMode << 4) | position);
    std::vector<unsigned char> bytes = {
      static_cast<unsigned char>(0xB0 | (kLedMidiChannel & 0x0F)),
      static_cast<unsigned char>(kEncoderLedRingCcStart + ch),
      value,
    };
    try {
      midi_out_->sendMessage(&bytes);
    } catch (const RtMidiError & e) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
        "MIDI out (encoder ring) failed on ch %zu: %s", ch, e.what());
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
  // [0]=Rec, [1]=Solo, [2]=Mute, [3]=Select.
  std::array<std::array<bool, kNumChannels>, kNumButtonKinds> button_state_{};
  // Encoder rotation counter (0..kEncoderRingMaxPosition), mirrored on LED ring.
  std::array<uint8_t, kNumChannels> encoder_count_{};
  // Encoder push toggle. Internal-only; not exposed externally.
  std::array<bool, kNumChannels> encoder_push_state_{};
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
