#pragma once

#include <memory>
#include <string>
#include <vector>

#include "platform/common.h"
#include "video.h"

namespace video {

  struct encoder_platform_formats_gst : encoder_platform_formats_t {
    encoder_platform_formats_gst(
      platf::mem_type_e dev_type,
      platf::pix_fmt_e pix_fmt_8bit,
      platf::pix_fmt_e pix_fmt_10bit,
      platf::pix_fmt_e pix_fmt_yuv444_8bit,
      platf::pix_fmt_e pix_fmt_yuv444_10bit
    ) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = pix_fmt_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = pix_fmt_yuv444_10bit;
    }
  };

  /**
   * Holds a persistent encoder name string so the std::string_view
   * in encoder_t remains valid.
   */
  struct gst_encoder_name_holder {
    std::string name;
  };

  class gst_encode_device_t : public platf::encode_device_t {
  public:
    gst_encode_device_t() = default;
    int convert(platf::img_t &img) override;

    std::vector<uint8_t> nv12_buffer;
    int frame_width = 0;
    int frame_height = 0;
  };

  class gst_encode_session_t : public encode_session_t {
  public:
    gst_encode_session_t(
      void *pipeline,
      void *appsrc,
      void *appsink,
      int codec,
      int width,
      int height,
      int framerate
    );

    ~gst_encode_session_t() override;

    int convert(platf::img_t &img) override;
    void request_idr_frame() override;
    void request_normal_frame() override;
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override;

    int encode_frame(
      int64_t frame_nr,
      safe::mail_raw_t::queue_t<packet_t> &packets,
      void *channel_data,
      std::optional<std::chrono::steady_clock::time_point> frame_timestamp
    );

    bool has_error() const { return pipeline_error; }

  private:
    void *pipeline;
    void *appsrc;
    void *appsink;
    int codec;
    int width;
    int height;
    int framerate;
    bool force_idr;
    bool pipeline_error;
    guint bus_watch_id;
    void *sws_ctx;
    std::vector<uint8_t> nv12_buffer;
  };

  struct gst_encoder_candidate {
    std::string element_name;
    std::string display_name;
    int priority;
    int codec;
    bool is_hardware;
  };

  bool gst_startup();

  std::vector<gst_encoder_candidate> discover_gst_encoders();

  /**
   * Build encoder_t + persistent name holder from a candidate.
   * The name holder ensures encoder.name (string_view) stays valid.
   */
  struct gst_encoder_build_result {
    std::unique_ptr<encoder_t> encoder;
    std::unique_ptr<gst_encoder_name_holder> name_holder;
  };

  gst_encoder_build_result build_gst_encoder(const gst_encoder_candidate &cand);

  std::vector<encoder_t *> register_gst_encoders();

  std::unique_ptr<gst_encode_session_t> make_gst_encode_session(
    const encoder_t &encoder,
    const config_t &config
  );

}  // namespace video