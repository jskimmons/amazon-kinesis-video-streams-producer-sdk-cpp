#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <string.h>
#include <chrono>
#include <Logger.h>
#include "KinesisVideoProducer.h"
#include <vector>
#include <stdlib.h>

using namespace std;
using namespace com::amazonaws::kinesis::video;
using namespace log4cplus;

#ifdef __cplusplus
extern "C" {
#endif

int gstreamer_init(int, char **);

#ifdef __cplusplus
}
#endif

LOGGER_TAG("com.amazonaws.kinesis.video.gstreamer");

#define ACCESS_KEY_ENV_VAR "AWS_ACCESS_KEY_ID"
#define SECRET_KEY_ENV_VAR "AWS_SECRET_ACCESS_KEY"
#define SESSION_TOKEN_ENV_VAR "AWS_SESSION_TOKEN"
#define DEFAULT_REGION_ENV_VAR "AWS_DEFAULT_REGION"

#define DEFAULT_RETENTION_PERIOD_HOURS 2
#define DEFAULT_KMS_KEY_ID ""
#define DEFAULT_STREAMING_TYPE STREAMING_TYPE_REALTIME
#define DEFAULT_CONTENT_TYPE "video/h264"
#define DEFAULT_MAX_LATENCY_SECONDS 60
#define DEFAULT_FRAGMENT_DURATION_MILLISECONDS 2000
#define DEFAULT_TIMECODE_SCALE_MILLISECONDS 1
#define DEFAULT_KEY_FRAME_FRAGMENTATION TRUE
#define DEFAULT_FRAME_TIMECODES TRUE
#define DEFAULT_ABSOLUTE_FRAGMENT_TIMES TRUE
#define DEFAULT_FRAGMENT_ACKS TRUE
#define DEFAULT_RESTART_ON_ERROR TRUE
#define DEFAULT_RECALCULATE_METRICS TRUE
#define DEFAULT_STREAM_FRAMERATE 25
#define DEFAULT_AVG_BANDWIDTH_BPS (4 * 1024 * 1024)
#define DEFAULT_BUFFER_DURATION_SECONDS 180
#define DEFAULT_REPLAY_DURATION_SECONDS 40
#define DEFAULT_CONNECTION_STALENESS_SECONDS 60
#define DEFAULT_CODEC_ID "V_MPEG4/ISO/AVC"
#define DEFAULT_TRACKNAME "kinesis_video"

namespace com { namespace amazonaws { namespace kinesis { namespace video {

class SampleClientCallbackProvider : public ClientCallbackProvider {
public:

    UINT64 getCallbackCustomData() override {
        return reinterpret_cast<UINT64> (this);
    }

    StorageOverflowPressureFunc getStorageOverflowPressureCallback() override {
        return storageOverflowPressure;
    }

    static STATUS storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes);
};

class SampleStreamCallbackProvider : public StreamCallbackProvider {
public:

    UINT64 getCallbackCustomData() override {
        return reinterpret_cast<UINT64> (this);
    }

    StreamConnectionStaleFunc getStreamConnectionStaleCallback() override {
        return streamConnectionStaleHandler;
    };

    StreamErrorReportFunc getStreamErrorReportCallback() override {
        return streamErrorReportHandler;
    };

    DroppedFrameReportFunc getDroppedFrameReportCallback() override {
        return droppedFrameReportHandler;
    };

private:
    static STATUS
    streamConnectionStaleHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                 UINT64 last_buffering_ack);

    static STATUS
    streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 errored_timecode,
                             STATUS status_code);

    static STATUS
    droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                              UINT64 dropped_frame_timecode);
};

class SampleCredentialProvider : public StaticCredentialProvider {
    // Test rotation period is 40 second for the grace period.
    const std::chrono::duration<uint64_t> ROTATION_PERIOD = std::chrono::seconds(2400);
public:
    SampleCredentialProvider(const Credentials &credentials) :
            StaticCredentialProvider(credentials) {}

    void updateCredentials(Credentials &credentials) override {
        // Copy the stored creds forward
        credentials = credentials_;

        // Update only the expiration
        auto now_time = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch());
        auto expiration_seconds = now_time + ROTATION_PERIOD;
        credentials.setExpiration(std::chrono::seconds(expiration_seconds.count()));
        LOG_INFO("New credentials expiration is " << credentials.getExpiration().count());
    }
};

class SampleDeviceInfoProvider : public DefaultDeviceInfoProvider {
public:
    device_info_t getDeviceInfo() override {
        auto device_info = DefaultDeviceInfoProvider::getDeviceInfo();
        // Set the storage size to 256mb
        device_info.storageInfo.storageSize = 512 * 1024 * 1024;
        return device_info;
    }
};

STATUS
SampleClientCallbackProvider::storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes) {
    UNUSED_PARAM(custom_handle);
    LOG_WARN("Reporting storage overflow. Bytes remaining " << remaining_bytes);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamConnectionStaleHandler(UINT64 custom_data,
                                                                  STREAM_HANDLE stream_handle,
                                                                  UINT64 last_buffering_ack) {
    LOG_WARN("Reporting stream stale. Last ACK received " << last_buffering_ack);
    return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                       UINT64 errored_timecode, STATUS status_code) {
    LOG_ERROR("Reporting stream error. Errored timecode: " << errored_timecode << " Status: "
                                                           << status_code);
    return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                        UINT64 dropped_frame_timecode) {
    LOG_WARN("Reporting dropped frame. Frame timecode " << dropped_frame_timecode);
    return STATUS_SUCCESS;
}

}  // namespace video
}  // namespace kinesis
}  // namespace amazonaws
}  // namespace com;

unique_ptr<Credentials> credentials_;

typedef struct _CustomData {
    GstElement *pipeline, *source, *source_filter, *encoder, *filter, *appsink, *video_convert, *h264parse;
    GstBus *bus;
    GMainLoop *main_loop;
    unique_ptr<KinesisVideoProducer> kinesis_video_producer;
    shared_ptr<KinesisVideoStream> kinesis_video_stream;
    bool stream_started;
    bool h264_stream_supported;
} CustomData;

void create_kinesis_video_frame(Frame *frame, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags,
                                void *data, size_t len) {
    frame->flags = flags;
    frame->decodingTs = static_cast<UINT64>(dts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
    frame->presentationTs = static_cast<UINT64>(pts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
    frame->duration = 10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    frame->size = static_cast<UINT32>(len);
    frame->frameData = reinterpret_cast<PBYTE>(data);
}

bool put_frame(shared_ptr<KinesisVideoStream> kinesis_video_stream, void *data, size_t len, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags) {
    Frame frame;
    create_kinesis_video_frame(&frame, pts, dts, flags, data, len);
    return kinesis_video_stream->putFrame(frame);
}

static GstFlowReturn on_new_sample(GstElement *sink, CustomData *data) {
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK (sink));
    GstCaps* gstcaps  = (GstCaps*) gst_sample_get_caps(sample);
    GstStructure * gststructforcaps = gst_caps_get_structure(gstcaps, 0);

    if (!data->stream_started) {
        data->stream_started = true;
        const GValue *gstStreamFormat = gst_structure_get_value(gststructforcaps, "codec_data");
        gchar *cpd = gst_value_serialize(gstStreamFormat);
        data->kinesis_video_stream->start(std::string(cpd));
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    bool isDroppable =  GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
                        GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY);
    if (!isDroppable) {
        bool isHeader = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_HEADER);
        // drop if buffer contains header only and has invalid timestamp
        if (!(isHeader && (!GST_BUFFER_PTS_IS_VALID(buffer) || !GST_BUFFER_DTS_IS_VALID(buffer)))) {
            size_t buffer_size = gst_buffer_get_size(buffer);
            uint8_t *frame_data = new uint8_t[buffer_size];
            gst_buffer_extract(buffer, 0, frame_data, buffer_size);

            bool delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            FRAME_FLAGS kinesis_video_flags;
            if(!delta) {
                // Safeguard stream and playback in case of h264 keyframes comes with different PTS and DTS
                if (data->h264_stream_supported) {
                    buffer->pts = buffer->dts;
                }
                kinesis_video_flags = FRAME_FLAG_KEY_FRAME;
            } else {
                kinesis_video_flags = FRAME_FLAG_NONE;
            }

            if (false == put_frame(data->kinesis_video_stream, frame_data, buffer_size, std::chrono::nanoseconds(buffer->pts),
                                   std::chrono::nanoseconds(buffer->dts), kinesis_video_flags)) {
                g_printerr("Dropped frame!\n");
            }

            delete[] frame_data;
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static bool format_supported_by_source(GstCaps *src_caps, GstCaps *query_caps, int width, int height, int framerate) {
    gst_caps_set_simple(query_caps,
                        "width", G_TYPE_INT, width,
                        "height", G_TYPE_INT, height,
                        "framerate", GST_TYPE_FRACTION, framerate, 1,
                        NULL);
    bool is_match = gst_caps_can_intersect(query_caps, src_caps);

    // in case the camera has fps as 10000000/333333
    if(!is_match) {
        gst_caps_set_simple(query_caps,
                            "framerate", GST_TYPE_FRACTION_RANGE, framerate, 1, framerate+1, 1,
                            NULL);
        is_match = gst_caps_can_intersect(query_caps, src_caps);
    }

    return is_match;
}

static bool resolution_supported(GstCaps *src_caps, GstCaps *query_caps_raw, GstCaps *query_caps_h264,
                                 CustomData &data, int width, int height, int framerate) {
    if (query_caps_h264 && format_supported_by_source(src_caps, query_caps_h264, width, height, framerate)) {
        data.h264_stream_supported = true;
    } else if (query_caps_raw && format_supported_by_source(src_caps, query_caps_raw, width, height, framerate)) {
        data.h264_stream_supported = false;
    } else {
        return false;
    }
    return true;
}

/* This function is called when an error message is posted on the bus */
static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
    GError *err;
    gchar *debug_info;

    /* Print error details on the screen */
    gst_message_parse_error(msg, &err, &debug_info);
    g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
    g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
    g_clear_error(&err);
    g_free(debug_info);

    g_main_loop_quit(data->main_loop);
}

void kinesis_video_init(CustomData *data, char *stream_name) {
    unique_ptr<DeviceInfoProvider> device_info_provider = make_unique<SampleDeviceInfoProvider>();
    unique_ptr<ClientCallbackProvider> client_callback_provider = make_unique<SampleClientCallbackProvider>();
    unique_ptr<StreamCallbackProvider> stream_callback_provider = make_unique<SampleStreamCallbackProvider>();

    char const *accessKey;
    char const *secretKey;
    char const *sessionToken;
    char const *defaultRegion;
    string defaultRegionStr;
    string sessionTokenStr;
    if (nullptr == (accessKey = getenv(ACCESS_KEY_ENV_VAR))) {
        accessKey = "AccessKey";
    }

    if (nullptr == (secretKey = getenv(SECRET_KEY_ENV_VAR))) {
        secretKey = "SecretKey";
    }

    if (nullptr == (sessionToken = getenv(SESSION_TOKEN_ENV_VAR))) {
        sessionTokenStr = "";
    } else {
        sessionTokenStr = string(sessionToken);
    }

    if (nullptr == (defaultRegion = getenv(DEFAULT_REGION_ENV_VAR))) {
        defaultRegionStr = DEFAULT_AWS_REGION;
    } else {
        defaultRegionStr = string(defaultRegion);
    }

    credentials_ = make_unique<Credentials>(string(accessKey),
                                            string(secretKey),
                                            sessionTokenStr,
                                            std::chrono::seconds(180));
    unique_ptr<CredentialProvider> credential_provider = make_unique<SampleCredentialProvider>(*credentials_.get());

    data->kinesis_video_producer = KinesisVideoProducer::createSync(move(device_info_provider),
                                                                    move(client_callback_provider),
                                                                    move(stream_callback_provider),
                                                                    move(credential_provider),
                                                                    defaultRegionStr);

    LOG_DEBUG("Client is ready");
    /* create a test stream */
    map<string, string> tags;
    char tag_name[MAX_TAG_NAME_LEN];
    char tag_val[MAX_TAG_VALUE_LEN];
    SPRINTF(tag_name, "piTag");
    SPRINTF(tag_val, "piValue");
    auto stream_definition = make_unique<StreamDefinition>(stream_name,
                                                           hours(DEFAULT_RETENTION_PERIOD_HOURS),
                                                           &tags,
                                                           DEFAULT_KMS_KEY_ID,
                                                           DEFAULT_STREAMING_TYPE,
                                                           DEFAULT_CONTENT_TYPE,
                                                           duration_cast<milliseconds> (seconds(DEFAULT_MAX_LATENCY_SECONDS)),
                                                           milliseconds(DEFAULT_FRAGMENT_DURATION_MILLISECONDS),
                                                           milliseconds(DEFAULT_TIMECODE_SCALE_MILLISECONDS),
                                                           DEFAULT_KEY_FRAME_FRAGMENTATION,
                                                           DEFAULT_FRAME_TIMECODES,
                                                           DEFAULT_ABSOLUTE_FRAGMENT_TIMES,
                                                           DEFAULT_FRAGMENT_ACKS,
                                                           DEFAULT_RESTART_ON_ERROR,
                                                           DEFAULT_RECALCULATE_METRICS,
                                                           0,
                                                           DEFAULT_STREAM_FRAMERATE,
                                                           DEFAULT_AVG_BANDWIDTH_BPS,
                                                           seconds(DEFAULT_BUFFER_DURATION_SECONDS),
                                                           seconds(DEFAULT_REPLAY_DURATION_SECONDS),
                                                           seconds(DEFAULT_CONNECTION_STALENESS_SECONDS),
                                                           DEFAULT_CODEC_ID,
                                                           DEFAULT_TRACKNAME,
                                                           nullptr,
                                                           0);
    data->kinesis_video_stream = data->kinesis_video_producer->createStreamSync(move(stream_definition));

    LOG_DEBUG("Stream is ready");
}

int gstreamer_init(int argc, char* argv[]) {
    PropertyConfigurator::doConfigure("kvs_log_configuration");

    if (argc < 2) {
        LOG_ERROR(
                "Usage: AWS_ACCESS_KEY_ID=SAMPLEKEY AWS_SECRET_ACCESS_KEY=SAMPLESECRET ./kinesis_video_gstreamer_sample_app -w width -h height -f framerate -b bitrateInKBPS my-stream-name \n \
           or AWS_ACCESS_KEY_ID=SAMPLEKEY AWS_SECRET_ACCESS_KEY=SAMPLESECRET ./kinesis_video_gstreamer_sample_app my-stream-name");
        return 1;
    }

    CustomData data;
    GstStateChangeReturn ret;
    bool vtenc, isOnRpi;

    /* init data struct */
    memset(&data, 0, sizeof(data));

    /* init GStreamer */
    gst_init(&argc, &argv);

    /* init stream format */
    char stream_name[MAX_STREAM_NAME_LEN];
    int width = 0, height = 0, framerate = 30, bitrateInKBPS = 512;
    for (int i = 1; i < argc; i++) {
        if (i < argc - 1) {
            if ((0 == STRCMPI(argv[i], "-w")) ||
                (0 == STRCMPI(argv[i], "/w")) ||
                (0 == STRCMPI(argv[i], "--w"))) {
                // process the width
                if (STATUS_FAILED(STRTOI32(argv[i + 1], NULL, 10, &width))) {
                    return 1;
                }
            }
            else if ((0 == STRCMPI(argv[i], "-h")) ||
                     (0 == STRCMPI(argv[i], "/h")) ||
                     (0 == STRCMPI(argv[i], "--h"))) {
                // process the width
                if (STATUS_FAILED(STRTOI32(argv[i + 1], NULL, 10, &height))) {
                    return 1;
                }
            }
            else if ((0 == STRCMPI(argv[i], "-f")) ||
                     (0 == STRCMPI(argv[i], "/f")) ||
                     (0 == STRCMPI(argv[i], "--f"))) {
                // process the width
                if (STATUS_FAILED(STRTOI32(argv[i + 1], NULL, 10, &framerate))) {
                    return 1;
                }
            }
            else if ((0 == STRCMPI(argv[i], "-b")) ||
                     (0 == STRCMPI(argv[i], "/b")) ||
                     (0 == STRCMPI(argv[i], "--b"))) {
                // process the width
                if (STATUS_FAILED(STRTOI32(argv[i + 1], NULL, 10, &bitrateInKBPS))) {
                    return 1;
                }
            }
            // skip the index
            i++;
        }
        else if (0 == STRCMPI(argv[i], "-?") ||
                 0 == STRCMPI(argv[i], "--?") ||
                 0 == STRCMPI(argv[i], "--help")) {
            g_printerr("Invalid arguments\n");
            return 1;
        }
        else if (argv[i][0] == '/' ||
                 argv[i][0] == '-') {
            // Unknown option
            g_printerr("Invalid arguments\n");
            return 1;
        }
        else {
            // Assume it's the stream name
            STRNCPY(stream_name, argv[i], MAX_STREAM_NAME_LEN);
        }
    }

    /* init Kinesis Video */
    kinesis_video_init(&data, stream_name);

    if ((width == 0 && height != 0) || (width != 0 && height == 0)) {
        g_printerr("Invalid resolution\n");
        return 1;
    }

    /* create the elemnents */
    /*
       gst-launch-1.0 v4l2src device=/dev/video0 ! video/x-raw,format=I420,width=1280,height=720,framerate=15/1 ! x264enc pass=quant bframes=0 ! video/x-h264,profile=baseline,format=I420,width=1280,height=720,framerate=15/1 ! matroskamux ! filesink location=test.mkv
     */
    data.source_filter = gst_element_factory_make("capsfilter", "source_filter");
    data.filter = gst_element_factory_make("capsfilter", "encoder_filter");
    data.appsink = gst_element_factory_make("appsink", "appsink");
    data.h264parse = gst_element_factory_make("h264parse", "h264parse"); // needed to enforce avc stream format

    // Attempt to create vtenc encoder
    data.encoder = gst_element_factory_make("vtenc_h264_hw", "encoder");
    if (data.encoder) {
        data.source = gst_element_factory_make("autovideosrc", "source");
        vtenc = true;
    } else {
        // Failed creating vtenc - check pi hardware encoder
        data.encoder = gst_element_factory_make("omxh264enc", "encoder");
        if (data.encoder) {
            isOnRpi = true;
        } else {
            // - attempt x264enc
            data.encoder = gst_element_factory_make("x264enc", "encoder");
            isOnRpi = false;
        }
        data.source = gst_element_factory_make("v4l2src", "source");
        if (!data.source) {
            data.source = gst_element_factory_make("ksvideosrc", "source");
        }
        vtenc = false;
    }

    /* create an empty pipeline */
    data.pipeline = gst_pipeline_new("test-pipeline");

    if (!data.pipeline || !data.source || !data.source_filter || !data.encoder || !data.filter || !data.appsink || !data.h264parse) {
        g_printerr("Not all elements could be created.\n");
        return 1;
    }

    /* configure source */
    if (!vtenc) {
        g_object_set(G_OBJECT (data.source), "do-timestamp", TRUE, "device", "/dev/video0", NULL);
    }

    /* Determine whether device supports h264 encoding and select a streaming resolution supported by the device*/
    if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(data.source, GST_STATE_READY)) {
        g_printerr("Unable to set the source to ready state.\n");
        return 1;
    }

    GstPad *srcpad = gst_element_get_static_pad(data.source, "src");
    GstCaps *src_caps = gst_pad_query_caps(srcpad, NULL);
    gst_element_set_state(data.source, GST_STATE_NULL);

    GstCaps *query_caps_raw = gst_caps_new_simple("video/x-raw",
                                                  "width", G_TYPE_INT, width,
                                                  "height", G_TYPE_INT, height,
                                                  NULL);
    GstCaps *query_caps_h264 = gst_caps_new_simple("video/x-h264",
                                                   "width", G_TYPE_INT, width,
                                                   "height", G_TYPE_INT, height,
                                                   NULL);

    if (width != 0 && height != 0) {
        if (!resolution_supported(src_caps, query_caps_raw, query_caps_h264, data, width, height, framerate)) {
            g_printerr("Resolution %dx%d not supported by video source\n", width, height);
            return 1;
        }
    } else {
        vector<int> res_width = {640, 1280, 1920};
        vector<int> res_height = {480, 720, 1080};
        bool found_resolution = false;
        for (int i = 0; i < res_width.size(); i++) {
            width = res_width[i];
            height = res_height[i];
            if (resolution_supported(src_caps, query_caps_raw, query_caps_h264, data, width, height, framerate)) {
                found_resolution = true;
                break;
            }
        }
        if (!found_resolution) {
            g_printerr("Default list of resolutions (1920x1080, 1280x720, 640x480) are not supported by video source\n");
            return 1;
        }
    }

    gst_caps_unref(src_caps);
    gst_object_unref(srcpad);

    /* create the elemnents needed for the corresponding pipeline */
    if (!data.h264_stream_supported) {
        data.video_convert = gst_element_factory_make("videoconvert", "video_convert");

        if (!data.video_convert) {
            g_printerr("Not all elements could be created.\n");
            return 1;
        }
    }

    /* source filter */
    if (!data.h264_stream_supported) {
        gst_caps_set_simple(query_caps_raw,
                            "format", G_TYPE_STRING, "I420",
                            NULL);
        g_object_set(G_OBJECT (data.source_filter), "caps", query_caps_raw, NULL);
    } else {
        gst_caps_set_simple(query_caps_h264,
                            "stream-format", G_TYPE_STRING, "byte-stream",
                            "alignment", G_TYPE_STRING, "au",
                            NULL);
        g_object_set(G_OBJECT (data.source_filter), "caps", query_caps_h264, NULL);
    }
    gst_caps_unref(query_caps_h264);
    gst_caps_unref(query_caps_raw);

    /* configure encoder */
    if (!data.h264_stream_supported){
        if (vtenc) {
            g_object_set(G_OBJECT (data.encoder), "allow-frame-reordering", FALSE, "realtime", TRUE, "max-keyframe-interval",
                              45, "bitrate", bitrateInKBPS, NULL);
        } else if (isOnRpi) {
            g_object_set(G_OBJECT (data.encoder), "control-rate", 2, "target-bitrate", bitrateInKBPS*1000,
                "periodicty-idr", 45, "inline-header", FALSE, NULL);
        } else {
            g_object_set(G_OBJECT (data.encoder), "bframes", 0, "key-int-max", 45, "bitrate", bitrateInKBPS, NULL);
        }
    }


    /* configure filter */
    GstCaps *h264_caps = gst_caps_new_simple("video/x-h264",
                                             "stream-format", G_TYPE_STRING, "avc",
                                             "alignment", G_TYPE_STRING, "au",
                                             NULL);
    if (!data.h264_stream_supported) {
        gst_caps_set_simple(h264_caps, "profile", G_TYPE_STRING, "baseline",
                                        NULL);
    }
    g_object_set(G_OBJECT (data.filter), "caps", h264_caps, NULL);
    gst_caps_unref(h264_caps);

    /* configure appsink */
    g_object_set(G_OBJECT (data.appsink), "emit-signals", TRUE, "sync", FALSE, NULL);
    g_signal_connect(data.appsink, "new-sample", G_CALLBACK(on_new_sample), &data);

    /* build the pipeline */
    if (!data.h264_stream_supported) {
        gst_bin_add_many(GST_BIN (data.pipeline), data.source, data.video_convert, data.source_filter, data.encoder, data.h264parse, data.filter,
            data.appsink, NULL);
        if (!gst_element_link_many(data.source, data.video_convert, data.source_filter, data.encoder, data.h264parse, data.filter, data.appsink, NULL)) {
            g_printerr("Elements could not be linked.\n");
            gst_object_unref(data.pipeline);
            return 1;
        }
    } else {
        gst_bin_add_many(GST_BIN (data.pipeline), data.source, data.source_filter, data.h264parse, data.filter, data.appsink, NULL);
        if (!gst_element_link_many(data.source, data.source_filter, data.h264parse, data.filter, data.appsink, NULL)) {
            g_printerr("Elements could not be linked.\n");
            gst_object_unref(data.pipeline);
            return 1;
        }
    }


    /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
    data.bus = gst_element_get_bus(data.pipeline);
    gst_bus_add_signal_watch(data.bus);
    g_signal_connect (G_OBJECT(data.bus), "message::error", (GCallback) error_cb, &data);
    gst_object_unref(data.bus);

    /* start streaming */
    ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the playing state.\n");
        gst_object_unref(data.pipeline);
        return 1;
    }

    data.main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(data.main_loop);

    /* free resources */
    gst_element_set_state(data.pipeline, GST_STATE_NULL);
    gst_object_unref(data.pipeline);
    return 0;
}

int main(int argc, char* argv[]) {
    return gstreamer_init(argc, argv);
}
