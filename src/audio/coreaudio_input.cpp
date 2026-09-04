#include "hero_audio/coreaudio_input.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace hero_audio {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "CoreAudio callback counters must be lock-free");

[[nodiscard]] std::string status_description(OSStatus status) {
  const auto bits = static_cast<std::uint32_t>(status);
  const std::array<char, 5> fourcc{
      static_cast<char>((bits >> 24U) & 0xFFU),
      static_cast<char>((bits >> 16U) & 0xFFU),
      static_cast<char>((bits >> 8U) & 0xFFU),
      static_cast<char>(bits & 0xFFU),
      '\0',
  };
  const bool printable = std::all_of(fourcc.begin(), fourcc.begin() + 4,
                                     [](char value) { return value >= 32 && value <= 126; });
  return printable ? "'" + std::string(fourcc.data(), 4) + "' (" +
                         std::to_string(status) + ")"
                   : std::to_string(status);
}

void require_status(OSStatus status, const char *operation) {
  if (status != noErr) {
    throw std::runtime_error(std::string(operation) + " failed with CoreAudio status " +
                             status_description(status));
  }
}

[[nodiscard]] AudioDeviceID default_input_device() {
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = static_cast<UInt32>(sizeof(device));
  AudioObjectPropertyAddress address{
      kAudioHardwarePropertyDefaultInputDevice,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain,
  };
  require_status(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                            &size, &device),
                 "Read default input device");
  if (device == kAudioObjectUnknown) {
    throw std::runtime_error("macOS has no default audio input device");
  }
  return device;
}

[[nodiscard]] std::string input_device_name(AudioDeviceID device) {
  CFStringRef name = nullptr;
  UInt32 size = static_cast<UInt32>(sizeof(name));
  AudioObjectPropertyAddress address{
      kAudioObjectPropertyName,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain,
  };
  const OSStatus status =
      AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name);
  if (status != noErr || name == nullptr) {
    return "unknown-input-device";
  }

  std::array<char, 512> utf8{};
  const Boolean converted =
      CFStringGetCString(name, utf8.data(), static_cast<CFIndex>(utf8.size()),
                         kCFStringEncodingUTF8);
  CFRelease(name);
  return converted != 0 ? std::string(utf8.data()) : "unprintable-input-device";
}

} // namespace

class CoreAudioInput::Impl {
public:
  explicit Impl(LiveAudioHopQueue &queue) : queue_(queue) {
    try {
      configure();
    } catch (...) {
      dispose();
      throw;
    }
  }

  ~Impl() { dispose(); }

  void start() {
    if (running_) {
      throw std::logic_error("CoreAudio input is already running");
    }
    if (started_once_) {
      throw std::logic_error("CoreAudio input instances cannot be restarted");
    }
    require_status(AudioOutputUnitStart(audio_unit_), "Start CoreAudio input");
    running_ = true;
    started_once_ = true;
  }

  void stop() noexcept {
    if (running_) {
      static_cast<void>(AudioOutputUnitStop(audio_unit_));
      running_ = false;
    }
  }

  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] double sample_rate_hz() const noexcept { return sample_rate_hz_; }
  [[nodiscard]] std::uint32_t source_channel_count() const noexcept {
    return source_channel_count_;
  }
  [[nodiscard]] std::uint32_t maximum_frames_per_slice() const noexcept {
    return maximum_frames_per_slice_;
  }
  [[nodiscard]] std::uint32_t device_id() const noexcept { return device_id_; }
  [[nodiscard]] const std::string &device_name() const noexcept { return device_name_; }

  [[nodiscard]] CoreAudioCaptureStats stats() const noexcept {
    return CoreAudioCaptureStats{
        .callback_count = callback_count_.load(std::memory_order_relaxed),
        .input_timeline_frame_count =
            input_timeline_frame_count_.load(std::memory_order_relaxed),
        .rendered_frame_count = rendered_frame_count_.load(std::memory_order_relaxed),
        .enqueued_hop_count = enqueued_hop_count_.load(std::memory_order_relaxed),
        .dropped_hop_count = dropped_hop_count_.load(std::memory_order_relaxed),
        .render_error_count = render_error_count_.load(std::memory_order_relaxed),
        .partial_sample_count = partial_sample_count_atomic_.load(std::memory_order_relaxed),
        .last_render_status = last_render_status_.load(std::memory_order_relaxed),
    };
  }

private:
  static OSStatus input_callback(void *context, AudioUnitRenderActionFlags *action_flags,
                                 const AudioTimeStamp *timestamp, UInt32,
                                 UInt32 frame_count, AudioBufferList *) noexcept {
    if (context == nullptr || action_flags == nullptr || timestamp == nullptr) {
      return kAudio_ParamError;
    }
    return static_cast<Impl *>(context)->render(action_flags, timestamp, frame_count);
  }

  void configure() {
    AudioComponentDescription description{};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_HALOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (component == nullptr) {
      throw std::runtime_error("Unable to find the macOS HAL output AudioUnit");
    }
    require_status(AudioComponentInstanceNew(component, &audio_unit_),
                   "Create HAL AudioUnit");

    UInt32 enabled = 1;
    require_status(AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                                        kAudioUnitScope_Input, 1, &enabled,
                                        static_cast<UInt32>(sizeof(enabled))),
                   "Enable HAL input bus");
    UInt32 disabled = 0;
    require_status(AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                                        kAudioUnitScope_Output, 0, &disabled,
                                        static_cast<UInt32>(sizeof(disabled))),
                   "Disable HAL output bus");

    device_id_ = default_input_device();
    device_name_ = input_device_name(device_id_);
    require_status(AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_CurrentDevice,
                                        kAudioUnitScope_Global, 0, &device_id_,
                                        static_cast<UInt32>(sizeof(device_id_))),
                   "Select default input device");

    AudioStreamBasicDescription hardware_format{};
    UInt32 format_size = static_cast<UInt32>(sizeof(hardware_format));
    require_status(AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                                        kAudioUnitScope_Input, 1, &hardware_format,
                                        &format_size),
                   "Read input device stream format");
    if (!std::isfinite(hardware_format.mSampleRate) || hardware_format.mSampleRate <= 0.0) {
      throw std::runtime_error("Input device reported an invalid sample rate");
    }
    sample_rate_hz_ = hardware_format.mSampleRate;
    source_channel_count_ = hardware_format.mChannelsPerFrame;
    if (source_channel_count_ == 0) {
      throw std::runtime_error("Input device reported zero channels");
    }

    AudioStreamBasicDescription client_format{};
    client_format.mSampleRate = sample_rate_hz_;
    client_format.mFormatID = kAudioFormatLinearPCM;
    client_format.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    client_format.mBytesPerPacket = static_cast<UInt32>(sizeof(float));
    client_format.mFramesPerPacket = 1;
    client_format.mBytesPerFrame = static_cast<UInt32>(sizeof(float));
    client_format.mChannelsPerFrame = 1;
    client_format.mBitsPerChannel = 32;
    require_status(AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                                        kAudioUnitScope_Output, 1, &client_format,
                                        static_cast<UInt32>(sizeof(client_format))),
                   "Set mono float32 client format");

    // The consumer supplies render_buffer_ to AudioUnitRender, so the AudioUnit
    // must not allocate or replace the data pointer from its real-time thread.
    UInt32 should_allocate = 0;
    require_status(AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_ShouldAllocateBuffer,
                                        kAudioUnitScope_Output, 1, &should_allocate,
                                        static_cast<UInt32>(sizeof(should_allocate))),
                   "Disable HAL render-buffer allocation");

    UInt32 maximum_frames = 0;
    UInt32 maximum_frames_size = static_cast<UInt32>(sizeof(maximum_frames));
    require_status(AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_MaximumFramesPerSlice,
                                        kAudioUnitScope_Global, 0, &maximum_frames,
                                        &maximum_frames_size),
                   "Read maximum CoreAudio frames per slice");
    if (maximum_frames == 0) {
      throw std::runtime_error("CoreAudio maximum frames per slice is zero");
    }
    maximum_frames_per_slice_ = maximum_frames;
    render_buffer_.resize(maximum_frames_per_slice_);

    render_buffers_.mNumberBuffers = 1;
    render_buffers_.mBuffers[0].mNumberChannels = 1;
    render_buffers_.mBuffers[0].mDataByteSize =
        maximum_frames_per_slice_ * static_cast<UInt32>(sizeof(float));
    render_buffers_.mBuffers[0].mData = render_buffer_.data();

    AURenderCallbackStruct callback{
        .inputProc = &Impl::input_callback,
        .inputProcRefCon = this,
    };
    require_status(AudioUnitSetProperty(audio_unit_,
                                        kAudioOutputUnitProperty_SetInputCallback,
                                        kAudioUnitScope_Global, 0, &callback,
                                        static_cast<UInt32>(sizeof(callback))),
                   "Install CoreAudio input callback");
    require_status(AudioUnitInitialize(audio_unit_), "Initialize CoreAudio input");
    initialized_ = true;
  }

  [[nodiscard]] OSStatus render(AudioUnitRenderActionFlags *action_flags,
                                const AudioTimeStamp *timestamp,
                                UInt32 frame_count) noexcept {
    callback_count_.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t callback_host_time = AudioGetCurrentHostTime();
    if (frame_count > maximum_frames_per_slice_) {
      render_error_count_.fetch_add(1, std::memory_order_relaxed);
      last_render_status_.store(kAudio_ParamError, std::memory_order_relaxed);
      absolute_sample_cursor_ += frame_count;
      input_timeline_frame_count_.store(absolute_sample_cursor_,
                                        std::memory_order_relaxed);
      partial_sample_count_ = 0;
      partial_sample_count_atomic_.store(0, std::memory_order_relaxed);
      return kAudio_ParamError;
    }

    render_buffers_.mBuffers[0].mDataByteSize =
        frame_count * static_cast<UInt32>(sizeof(float));
    render_buffers_.mBuffers[0].mData = render_buffer_.data();
    const OSStatus status = AudioUnitRender(audio_unit_, action_flags, timestamp, 1,
                                            frame_count, &render_buffers_);
    if (status != noErr) {
      render_error_count_.fetch_add(1, std::memory_order_relaxed);
      last_render_status_.store(status, std::memory_order_relaxed);
      absolute_sample_cursor_ += frame_count;
      input_timeline_frame_count_.store(absolute_sample_cursor_,
                                        std::memory_order_relaxed);
      partial_sample_count_ = 0;
      partial_sample_count_atomic_.store(0, std::memory_order_relaxed);
      return status;
    }

    rendered_frame_count_.fetch_add(frame_count, std::memory_order_relaxed);
    aggregate_rendered_samples(render_buffer_.data(), frame_count, callback_host_time);
    absolute_sample_cursor_ += frame_count;
    input_timeline_frame_count_.store(absolute_sample_cursor_,
                                      std::memory_order_relaxed);
    return noErr;
  }

  void aggregate_rendered_samples(const float *samples, std::size_t sample_count,
                                  std::uint64_t callback_host_time) noexcept {
    std::size_t offset = 0;
    while (offset < sample_count) {
      if (partial_sample_count_ == 0) {
        partial_first_sample_index_ = absolute_sample_cursor_ + offset;
      }
      const auto copy_count = std::min(kLiveHopSize - partial_sample_count_,
                                       sample_count - offset);
      std::copy_n(samples + offset, copy_count,
                  partial_hop_.begin() + static_cast<std::ptrdiff_t>(partial_sample_count_));
      partial_sample_count_ += copy_count;
      offset += copy_count;

      if (partial_sample_count_ == kLiveHopSize) {
        const std::uint64_t sequence = next_hop_sequence_++;
        const bool enqueued = queue_.try_push(
            std::span<const float, kLiveHopSize>(partial_hop_), sequence,
            partial_first_sample_index_, callback_host_time);
        if (enqueued) {
          enqueued_hop_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
          dropped_hop_count_.fetch_add(1, std::memory_order_relaxed);
        }
        partial_sample_count_ = 0;
      }
    }
    partial_sample_count_atomic_.store(static_cast<std::uint32_t>(partial_sample_count_),
                                       std::memory_order_relaxed);
  }

  void dispose() noexcept {
    stop();
    if (initialized_) {
      static_cast<void>(AudioUnitUninitialize(audio_unit_));
      initialized_ = false;
    }
    if (audio_unit_ != nullptr) {
      static_cast<void>(AudioComponentInstanceDispose(audio_unit_));
      audio_unit_ = nullptr;
    }
  }

  LiveAudioHopQueue &queue_;
  AudioUnit audio_unit_{nullptr};
  AudioBufferList render_buffers_{};
  std::vector<float> render_buffer_;
  std::array<float, kLiveHopSize> partial_hop_{};
  std::size_t partial_sample_count_{};
  std::uint64_t partial_first_sample_index_{};
  std::uint64_t absolute_sample_cursor_{};
  std::uint64_t next_hop_sequence_{};
  double sample_rate_hz_{};
  std::uint32_t source_channel_count_{};
  std::uint32_t maximum_frames_per_slice_{};
  AudioDeviceID device_id_{kAudioObjectUnknown};
  std::string device_name_;
  bool initialized_{};
  bool running_{};
  bool started_once_{};

  std::atomic<std::uint64_t> callback_count_{0};
  std::atomic<std::uint64_t> input_timeline_frame_count_{0};
  std::atomic<std::uint64_t> rendered_frame_count_{0};
  std::atomic<std::uint64_t> enqueued_hop_count_{0};
  std::atomic<std::uint64_t> dropped_hop_count_{0};
  std::atomic<std::uint64_t> render_error_count_{0};
  std::atomic<std::uint32_t> partial_sample_count_atomic_{0};
  std::atomic<std::int32_t> last_render_status_{0};
};

CoreAudioInput::CoreAudioInput(LiveAudioHopQueue &queue)
    : impl_(std::make_unique<Impl>(queue)) {}

CoreAudioInput::~CoreAudioInput() = default;

void CoreAudioInput::start() { impl_->start(); }

void CoreAudioInput::stop() noexcept { impl_->stop(); }

bool CoreAudioInput::running() const noexcept { return impl_->running(); }

double CoreAudioInput::sample_rate_hz() const noexcept { return impl_->sample_rate_hz(); }

std::uint32_t CoreAudioInput::source_channel_count() const noexcept {
  return impl_->source_channel_count();
}

std::uint32_t CoreAudioInput::maximum_frames_per_slice() const noexcept {
  return impl_->maximum_frames_per_slice();
}

std::uint32_t CoreAudioInput::device_id() const noexcept { return impl_->device_id(); }

const std::string &CoreAudioInput::device_name() const noexcept {
  return impl_->device_name();
}

CoreAudioCaptureStats CoreAudioInput::stats() const noexcept { return impl_->stats(); }

std::uint64_t coreaudio_current_host_time() noexcept { return AudioGetCurrentHostTime(); }

double coreaudio_host_time_delta_ms(std::uint64_t earlier,
                                    std::uint64_t later) noexcept {
  if (later < earlier) {
    return 0.0;
  }
  return static_cast<double>(AudioConvertHostTimeToNanos(later - earlier)) / 1.0e6;
}

} // namespace hero_audio
