#include "gst_encoder.h"

#include <atomic>
#include <cstring>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "config.h"
#include "logging.h"

namespace video {

  namespace {

    std::atomic<bool> gst_inited{false};

    void ensure_gst_init() {
      bool expected = false;
      if (gst_inited.compare_exchange_strong(expected, true)) {
        gst_init(nullptr, nullptr);
      }
    }

    bool match_any(const std::string &name, std::initializer_list<const char *> patterns) {
      for (auto p : patterns) {
        if (name.find(p) != std::string::npos) {
          return true;
        }
      }
      return false;
    }

    bool is_hw(const std::string &name) {
      return match_any(name, {
        "vaapi", "nvenc", "nvh264", "nvh265", "nvav1",
        "qsv", "msdk", "amf", "v4l2"
      });
    }

    std::string display_name(const std::string &ename) {
      if (match_any(ename, {"vah264", "vaapih264"})) return "VA-API H.264";
      if (match_any(ename, {"vah265", "vaapih265"})) return "VA-API H.265";
      if (match_any(ename, {"nvh264", "nvenc_h264"})) return "NVENC H.264";
      if (match_any(ename, {"nvh265", "nvenc_h265"})) return "NVENC H.265";
      if (match_any(ename, {"nvav1", "nvenc_av1"})) return "NVENC AV1";
      if (match_any(ename, {"qsvh264"})) return "QSV H.264";
      if (match_any(ename, {"qsvh265"})) return "QSV H.265";
      if (match_any(ename, {"qsvav1"})) return "QSV AV1";
      if (match_any(ename, {"msdkh264"})) return "MSDK H.264";
      if (match_any(ename, {"x264"})) return "x264";
      if (match_any(ename, {"svtav1"})) return "SVT-AV1";
      if (match_any(ename, {"aomav1", "av1enc"})) return "AV1 (aom)";
      if (match_any(ename, {"openh264"})) return "OpenH264";
      if (match_any(ename, {"avenc_h264"})) return "Libav H.264";
      if (match_any(ename, {"avenc_h265", "avenc_hevc"})) return "Libav H.265";
      if (match_any(ename, {"avenc_av1"})) return "Libav AV1";
      return ename;
    }

    gboolean bus_watch(GstBus *bus, GstMessage *msg, gpointer data) {
      auto session = static_cast<gst_encode_session_t *>(data);
      if (!session) return TRUE;

      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
          GError *err = nullptr;
          gchar *debug = nullptr;
          gst_message_parse_error(msg, &err, &debug);
          BOOST_LOG(error)
            << "GStreamer error: " << (err ? err->message : "?")
            << " [" << (debug ? debug : "") << "]";
          g_error_free(err);
          g_free(debug);
          break;
        }
        case GST_MESSAGE_WARNING: {
          GError *err = nullptr;
          gchar *debug = nullptr;
          gst_message_parse_warning(msg, &err, &debug);
          BOOST_LOG(warning)
            << "GStreamer warning: " << (err ? err->message : "?")
            << " [" << (debug ? debug : "") << "]";
          g_error_free(err);
          g_free(debug);
          break;
        }
        case GST_MESSAGE_EOS:
          BOOST_LOG(info) << "GStreamer EOS";
          break;
        default:
          break;
      }
      return TRUE;
    }

  } // anonymous namespace

  // ---- startup & discovery ----

  bool gst_startup() {
    ensure_gst_init();
    return true;
  }

  std::vector<gst_encoder_candidate> discover_gst_encoders() {
    ensure_gst_init();

    std::vector<gst_encoder_candidate> candidates;
    auto *registry = gst_registry_get();
    if (!registry) return candidates;

    GList *features = gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);
    if (!features) return candidates;

    for (GList *f = features; f; f = f->next) {
      auto *factory = GST_ELEMENT_FACTORY(f->data);
      if (!factory) continue;

      const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
      if (!name) continue;

      const char *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
      if (!klass) continue;
      std::string k(klass);
      if (k.find("Video") == std::string::npos || k.find("Encoder") == std::string::npos) {
        continue;
      }

      std::string ename(name);
      int codec = -1;
      if (match_any(ename, {"h264", "x264", "openh264"})) codec = 0;
      else if (match_any(ename, {"h265", "hevc", "x265"})) codec = 1;
      else if (match_any(ename, {"av1"})) codec = 2;
      else continue;

      int rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory));
      if (is_hw(ename)) rank += 1000;

      candidates.push_back({ename, display_name(ename), rank, codec, is_hw(ename)});
    }
    gst_plugin_feature_list_free(features);

    std::sort(candidates.begin(), candidates.end(),
      [](auto &a, auto &b) { return a.priority > b.priority; });

    return candidates;
  }

  gst_encoder_build_result build_gst_encoder(const gst_encoder_candidate &cand) {
    auto name_holder = std::make_unique<gst_encoder_name_holder>();
    name_holder->name = std::string("gst-") + cand.element_name;

    auto formats = std::make_unique<encoder_platform_formats_gst>(
      platf::mem_type_e::system,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010
    );

    encoder_t::codec_t sel;
    sel.name = cand.element_name;

    auto enc = std::make_unique<encoder_t>();
    enc->name = name_holder->name;
    enc->platform_formats = std::move(formats);

    switch (cand.codec) {
      case 0: enc->h264 = sel; break;
      case 1: enc->hevc = sel; break;
      case 2: enc->av1 = sel; break;
    }
    enc->flags = PARALLEL_ENCODING;

    return {std::move(enc), std::move(name_holder)};
  }

  std::vector<encoder_t *> register_gst_encoders() {
    static std::vector<std::unique_ptr<encoder_t>> encoders;
    static std::vector<std::unique_ptr<gst_encoder_name_holder>> holders;
    static std::vector<encoder_t *> ptrs;
    static bool done = false;

    if (done) return ptrs;
    done = true;

    auto candidates = discover_gst_encoders();
    for (auto &cand : candidates) {
      auto [enc, holder] = build_gst_encoder(cand);
      BOOST_LOG(info) << "GStreamer: found [" << enc->name << "]";
      ptrs.push_back(enc.get());
      encoders.push_back(std::move(enc));
      holders.push_back(std::move(holder));
    }
    return ptrs;
  }

  // ---- gst_encode_device_t ----

  int gst_encode_device_t::convert(platf::img_t &img) {
    if (!img.data || img.width <= 0 || img.height <= 0) return -1;
    frame_width = img.width;
    frame_height = img.height;

    size_t sz = frame_width * frame_height * 3 / 2;
    nv12_buffer.resize(sz);

    auto *sws = sws_getContext(frame_width, frame_height, AV_PIX_FMT_BGRA,
                               frame_width, frame_height, AV_PIX_FMT_NV12,
                               SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return -1;

    uint8_t *src[4] = {img.data, nullptr, nullptr, nullptr};
    int src_stride[4] = {img.row_pitch, 0, 0, 0};
    uint8_t *dst[4] = {nv12_buffer.data(), nv12_buffer.data() + frame_width * frame_height, nullptr, nullptr};
    int dst_stride[4] = {frame_width, frame_width, 0, 0};

    sws_scale(sws, src, src_stride, 0, frame_height, dst, dst_stride);
    sws_freeContext(sws);
    return 0;
  }

  // ---- gst_encode_session_t ----

  gst_encode_session_t::gst_encode_session_t(
    void *pipeline, void *appsrc, void *appsink,
    int codec, int width, int height, int framerate
  )
    : pipeline(pipeline), appsrc(appsrc), appsink(appsink)
    , codec(codec), width(width), height(height), framerate(framerate)
    , force_idr(false), pipeline_error(false), bus_watch_id(0)
    , sws_ctx(nullptr) {}

  gst_encode_session_t::~gst_encode_session_t() {
    if (sws_ctx) sws_freeContext((SwsContext *)sws_ctx);

    if (pipeline) {
      auto *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
      if (bus && bus_watch_id) {
        gst_bus_remove_watch(bus, bus_watch_id);
        gst_object_unref(bus);
      }
      gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
      gst_object_unref(GST_OBJECT(pipeline));
    }
    if (appsrc) gst_object_unref(GST_OBJECT(appsrc));
    if (appsink) gst_object_unref(GST_OBJECT(appsink));
  }

  int gst_encode_session_t::convert(platf::img_t &img) {
    if (pipeline_error || !img.data) return -1;

    // Convert BGRA -> NV12
    size_t nv12_size = width * height * 3 / 2;
    nv12_buffer.resize(nv12_size);

    auto *sws = (SwsContext *)sws_ctx;
    if (!sws) {
      sws = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                           width, height, AV_PIX_FMT_NV12,
                           SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
      sws_ctx = sws;
      if (!sws) return -1;
    }

    uint8_t *src[4] = {img.data, nullptr, nullptr, nullptr};
    int src_stride[4] = {img.row_pitch, 0, 0, 0};
    uint8_t *dst[4] = {nv12_buffer.data(), nv12_buffer.data() + width * height, nullptr, nullptr};
    int dst_stride[4] = {width, width, 0, 0};

    sws_scale(sws, src, src_stride, 0, height, dst, dst_stride);

    // Push to appsrc
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, nv12_size, nullptr);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    std::memcpy(map.data, nv12_buffer.data(), nv12_size);
    gst_buffer_unmap(buf, &map);

    GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
    if (force_idr) {
      GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);

    return (ret == GST_FLOW_OK) ? 0 : -1;
  }

  void gst_encode_session_t::request_idr_frame() { force_idr = true; }
  void gst_encode_session_t::request_normal_frame() { force_idr = false; }
  void gst_encode_session_t::invalidate_ref_frames(int64_t, int64_t) { request_idr_frame(); }

  int gst_encode_session_t::encode_frame(
    int64_t frame_nr,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp
  ) {
    if (pipeline_error) return -1;

    GstSample *sample = nullptr;
    g_signal_emit_by_name(appsink, "try-pull-sample", &sample);
    if (!sample) return 0;

    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (!buf) { gst_sample_unref(sample); return -1; }

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
      gst_sample_unref(sample);
      return -1;
    }

    bool idr = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    auto pkt = std::make_unique<packet_raw_generic>(
      std::vector<uint8_t>(map.data, map.data + map.size),
      frame_nr, idr
    );
    pkt->channel_data = channel_data;
    pkt->frame_timestamp = frame_timestamp;
    packets->raise(std::move(pkt));

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
    return 0;
  }

  // ---- factory ----

  std::unique_ptr<gst_encode_session_t> make_gst_encode_session(
    const encoder_t &encoder, const config_t &config
  ) {
    auto &info = encoder.codec_from_config(config);
    if (info.name.empty()) return nullptr;

    ensure_gst_init();

    std::string desc =
      "appsrc name=src ! videoconvert ! " +
      info.name + " ! appsink name=sink";

    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(desc.c_str(), &err);
    if (err) {
      BOOST_LOG(error) << "GStreamer pipeline [" << desc << "]: " << err->message;
      g_error_free(err);
      return nullptr;
    }

    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!src || !sink) {
      if (src) gst_object_unref(src);
      if (sink) gst_object_unref(sink);
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return nullptr;
    }

    // appsrc caps: NV12, client-specified size + framerate
    auto caps = gst_caps_new_simple("video/x-raw",
      "format", G_TYPE_STRING, "NV12",
      "width", G_TYPE_INT, config.width,
      "height", G_TYPE_INT, config.height,
      "framerate", GST_TYPE_FRACTION, config.framerate, 1,
      nullptr);
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);

    gst_app_sink_set_drop(GST_APP_SINK(sink), false);
    gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 2);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      gst_object_unref(src);
      gst_object_unref(sink);
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return nullptr;
    }

    auto session = std::make_unique<gst_encode_session_t>(
      pipeline, src, sink,
      config.videoFormat,
      config.width, config.height, config.framerate
    );

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    if (bus) {
      session->bus_watch_id = gst_bus_add_watch(bus, bus_watch, session.get());
      gst_object_unref(bus);
    }

    return session;
  }

} // namespace video