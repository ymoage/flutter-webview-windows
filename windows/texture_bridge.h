#pragma once

#include <windows.graphics.capture.h>
#include <wrl.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "graphics_context.h"

typedef struct {
  size_t width;
  size_t height;
} Size;

class TextureBridge {
 public:
  typedef std::function<void()> FrameAvailableCallback;
  typedef std::function<void(Size size)> SurfaceSizeChangedCallback;
  typedef std::chrono::duration<double, std::milli> FrameDuration;

  TextureBridge(GraphicsContext* graphics_context,
                ABI::Windows::UI::Composition::IVisual* visual);
  virtual ~TextureBridge();

  bool Start();
  void Stop();

  void SetOnFrameAvailable(FrameAvailableCallback callback) {
    frame_available_ = std::move(callback);
  }

  void SetOnSurfaceSizeChanged(SurfaceSizeChangedCallback callback) {
    surface_size_changed_ = std::move(callback);
  }

  void NotifySurfaceSizeChanged();
  void SetFpsLimit(std::optional<int> max_fps);

 protected:
  // ISSUES #26 / design 98: frame pipeline statistics. Aggregated and printed
  // once every 5 seconds; per-frame logging is deliberately avoided because it
  // would itself introduce jitter into the very thing being measured.
  void RecordArrived();
  void RecordMarked();
  void RecordPresented(double copy_ms);
  void MaybeReportStats();

  struct FrameStats {
    uint64_t arrived = 0;
    uint64_t marked = 0;
    uint64_t presented = 0;
    std::vector<double> intervals_ms;  // between accepted arrivals
    double copy_total_ms = 0.0;
    double copy_max_ms = 0.0;
    uint64_t copy_count = 0;
    std::optional<std::chrono::high_resolution_clock::time_point> last_arrival;
    std::chrono::high_resolution_clock::time_point window_start =
        std::chrono::high_resolution_clock::now();
  };
  FrameStats stats_;

  bool is_running_ = false;

  const GraphicsContext* graphics_context_;
  std::mutex mutex_;
  std::optional<FrameDuration> frame_duration_ = std::nullopt;

  FrameAvailableCallback frame_available_;
  SurfaceSizeChangedCallback surface_size_changed_;
  // ISSUES #28: size of the capture frame pool currently in use. The pool is
  // reconciled every frame against capture_item_->get_Size(); a one-shot flag
  // could latch a stale size read taken before the visual actually resized and
  // never retry, leaving the page stretched.
  ABI::Windows::Graphics::SizeInt32 pool_size_ = {0, 0};
  // ISSUES #28: arms a single diagnostic log for the next pool resize.
  bool log_next_pool_resize_ = false;
  winrt::com_ptr<ID3D11Texture2D> last_frame_;
  std::optional<std::chrono::high_resolution_clock::time_point>
      last_frame_timestamp_;

  winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>
      capture_item_;
  winrt::com_ptr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool>
      frame_pool_;
  winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession>
      capture_session_;

  EventRegistrationToken on_closed_token_ = {};
  EventRegistrationToken on_frame_arrived_token_ = {};

  virtual void StopInternal();
  void OnFrameArrived();
  bool ShouldDropFrame();

  // corresponds to DXGI_FORMAT_B8G8R8A8_UNORM
  static constexpr auto kPixelFormat = ABI::Windows::Graphics::DirectX::
      DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized;
};
