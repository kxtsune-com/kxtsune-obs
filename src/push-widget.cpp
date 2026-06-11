#include "pch.h"
#include "helpers.h"
#include <regex>
#include <optional>
#include <tuple>
#include "push-widget.h"
#include "output-config.h"
#include "protocols.h"
#include "obs-multi-rtmp.h"

#include "obs.hpp"

class IOBSOutputEventHandler
{
public:
    virtual void OnStarting() {}
    static void OnOutputStarting(void* x, calldata_t*)
    {
        static_cast<IOBSOutputEventHandler*>(x)->OnStarting();
    }

    virtual void OnStarted() {}
    static void OnOutputStarted(void* x, calldata_t*)
    {
        static_cast<IOBSOutputEventHandler*>(x)->OnStarted();
    }

    virtual void OnStopping() {}
    static void OnOutputStopping(void* x, calldata_t*)
    {
        static_cast<IOBSOutputEventHandler*>(x)->OnStopping();
    }

    virtual void OnStopped(int /*code*/) {}
    static void OnOutputStopped(void* x, calldata_t* param)
    {
        static_cast<IOBSOutputEventHandler*>(x)->OnStopped(calldata_int(param, "code"));
    }

    virtual void OnReconnect() {}
    static void OnOutputReconnect(void* x, calldata_t*)
    {
        static_cast<IOBSOutputEventHandler*>(x)->OnReconnect();
    }

    virtual void OnReconnected() {}
    static void OnOutputReconnected(void* x, calldata_t*)
    {
        static_cast<IOBSOutputEventHandler*>(x)->OnReconnected();
    }

    virtual void onDeactive() {}
    static void OnOutputDeactive(void* x, calldata_t*)
    {
        static_cast<IOBSOutputEventHandler*>(x)->onDeactive();
    }

    void SetMeAsHandler(obs_output_t* output)
    {
        auto sig = obs_output_get_signal_handler(output);
        if (!sig) return;
        signal_handler_connect(sig, "starting",         &OnOutputStarting,   this);
        signal_handler_connect(sig, "start",            &OnOutputStarted,    this);
        signal_handler_connect(sig, "reconnect",        &OnOutputReconnect,  this);
        signal_handler_connect(sig, "reconnect_success",&OnOutputReconnected,this);
        signal_handler_connect(sig, "stopping",         &OnOutputStopping,   this);
        signal_handler_connect(sig, "deactivate",       &OnOutputDeactive,   this);
        signal_handler_connect(sig, "stop",             &OnOutputStopped,    this);
    }

    void DisconnectSignals(obs_output_t* output)
    {
        auto sig = obs_output_get_signal_handler(output);
        if (!sig) return;
        signal_handler_disconnect(sig, "starting",         &OnOutputStarting,   this);
        signal_handler_disconnect(sig, "start",            &OnOutputStarted,    this);
        signal_handler_disconnect(sig, "reconnect",        &OnOutputReconnect,  this);
        signal_handler_disconnect(sig, "reconnect_success",&OnOutputReconnected,this);
        signal_handler_disconnect(sig, "stopping",         &OnOutputStopping,   this);
        signal_handler_disconnect(sig, "deactivate",       &OnOutputDeactive,   this);
        signal_handler_disconnect(sig, "stop",             &OnOutputStopped,    this);
    }
};


class PushWidgetImpl : public QObject, public PushWidget, public IOBSOutputEventHandler
{
    Q_OBJECT
    // PushWidget is a plain interface (no QObject); QObject comes first.

    std::string targetid_;
    OutputTargetConfigPtr config_;

    using clock = std::chrono::steady_clock;
    clock::time_point begin_time_;
    clock::time_point last_info_time_;
    uint64_t total_frames_ = 0;
    uint64_t total_bytes_ = 0;
    QTimer* timer_ = nullptr;

    obs_output_t* output_ = nullptr;
    bool using_main_video_encoder_ = false;
    bool using_main_audio_encoder_ = false;
    obs_view_t* scene_view_ = nullptr;
    bool isUseDelay_ = false;

    StatusCallback statusCallback_;

    void NotifyStatus(const std::string& msg) {
        if (statusCallback_)
            statusCallback_(msg);
    }

    // ── service ──────────────────────────────────────────────────────────────

    bool PrepareOutputService()
    {
        if (!output_) return false;
        ReleaseOutputService();

        auto conf = obs_data_create_from_json(config_->serviceParam.dump().c_str());
        if (!conf) return false;

        auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        if (!protocolInfo) {
            blog(LOG_ERROR, TAG "Invalid protocol \"%s\"", config_->protocol.c_str());
            obs_data_release(conf);
            return false;
        }

        auto service = obs_service_create(protocolInfo->serviceId, "multi-output-service", conf, nullptr);
        obs_data_release(conf);
        if (!service) return false;
        obs_output_set_service(output_, service);
        return true;
    }

    bool ReleaseOutputService()
    {
        if (!output_) return true;
        if (obs_output_active(output_)) return false;
        auto service = obs_output_get_service(output_);
        if (service) {
            obs_output_set_service(output_, nullptr);
            obs_service_release(service);
        }
        return true;
    }

    // ── encoder source ────────────────────────────────────────────────────────

    bool PrepareEncoderSource()
    {
        if (!output_) return false;

        if (!using_main_video_encoder_) {
            auto venc = obs_output_get_video_encoder(output_);
            if (!venc) return false;
            auto videoConfig = FindById(GlobalMultiOutputConfig().videoConfig,
                                        config_->videoConfig.value_or(""));
            if (!videoConfig || !videoConfig->outputScene.has_value()) {
                obs_encoder_set_video(venc, obs_get_video());
            } else {
                auto sceneName = *videoConfig->outputScene;
                OBSSourceAutoRelease scene = obs_get_source_by_name(sceneName.c_str());
                if (!scene) {
                    blog(LOG_ERROR, TAG "Output scene not found.");
                    return false;
                }
                ReleaseOutputSceneView();
                scene_view_ = obs_view_create();
                obs_view_set_source(scene_view_, 0, scene);
                obs_source_inc_active(scene);
                obs_encoder_set_video(venc, obs_view_add(scene_view_));
            }
        }

        if (!using_main_audio_encoder_) {
            auto aenc = obs_output_get_audio_encoder(output_, 0);
            if (!aenc) return false;
            obs_encoder_set_audio(aenc, obs_get_audio());

            auto audioConfig = FindById(GlobalMultiOutputConfig().audioConfig,
                                         config_->audioConfig.value_or(""));
            if (audioConfig) {
                for (auto& track : audioConfig->audioTracks) {
                    auto enc = obs_output_get_audio_encoder(output_, track->output_track);
                    if (enc) obs_encoder_set_audio(enc, obs_get_audio());
                }
            }
        }
        return true;
    }

    bool ReleaseOutputSceneView()
    {
        if (!scene_view_) return true;
        obs_view_remove(scene_view_);
        OBSSourceAutoRelease source = obs_view_get_source(scene_view_, 0);
        if (source) obs_source_dec_active(source);
        obs_view_set_source(scene_view_, 0, nullptr);
        obs_view_destroy(scene_view_);
        scene_view_ = nullptr;
        return true;
    }

    // ── encoder helpers ───────────────────────────────────────────────────────

    std::string VideoEncoderName() {
        return "multi-rtmp-venc" + config_->videoConfig.value_or("");
    }
    std::string AudioEncoderName(int track) {
        return "multi-rtmp-aenc" + config_->audioConfig.value_or("") + "-track-idx-" + std::to_string(track);
    }

    std::optional<std::tuple<int,int>> ParseResolution(const std::optional<std::string>& res) {
        if (!res.has_value()) return std::nullopt;
        std::regex pat(R"__(\s*(\d{1,5})\s*x\s*(\d{1,5})\s*)__");
        std::smatch m;
        if (std::regex_match(*res, m, pat))
            return {{ std::stoi(m[1]), std::stoi(m[2]) }};
        return std::nullopt;
    }

    OBSEncoder GetVideoEncoder()
    {
        auto config_id = config_->videoConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id.empty() || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
            using_main_video_encoder_ = true;
            return obs_output_get_video_encoder(out);
        }
        if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease out = obs_frontend_get_recording_output();
            using_main_video_encoder_ = true;
            return obs_output_get_video_encoder(out);
        }
        OBSEncoderAutoRelease enc = obs_get_encoder_by_name(VideoEncoderName().c_str());
        if (!enc) {
            auto vcfg = FindById(GlobalMultiOutputConfig().videoConfig, config_id);
            if (vcfg) {
                OBSDataAutoRelease s = obs_data_create_from_json(vcfg->encoderParams.dump().c_str());
                enc = obs_video_encoder_create(vcfg->encoderId.c_str(), VideoEncoderName().c_str(), s, nullptr);
                if (enc) {
                    auto wh = ParseResolution(vcfg->resolution);
                    if (wh) {
                        obs_encoder_set_gpu_scale_type(enc, OBS_SCALE_BICUBIC);
                        auto [w,h] = *wh;
                        obs_encoder_set_scaled_size(enc, w, h);
                    }
                    obs_encoder_set_frame_rate_divisor(enc, vcfg->fpsDenumerator);
                }
            } else {
                blog(LOG_ERROR, TAG "No video encoder config found.");
                config_->videoConfig = OBS_STREAMING_ENC_PLACEHOLDER;
                return GetVideoEncoder();
            }
        }
        using_main_video_encoder_ = false;
        return enc.Get();
    }

    OBSEncoder GetAudioEncoder(int trackIdx = 0, std::optional<int> mixerId = std::nullopt)
    {
        auto config_id = config_->audioConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id.empty() || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
            using_main_audio_encoder_ = true;
            return obs_output_get_audio_encoder(out, 0);
        }
        if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease out = obs_frontend_get_recording_output();
            using_main_audio_encoder_ = true;
            return obs_output_get_audio_encoder(out, 0);
        }
        OBSEncoderAutoRelease enc = obs_get_encoder_by_name(AudioEncoderName(trackIdx).c_str());
        if (!enc) {
            auto acfg = FindById(GlobalMultiOutputConfig().audioConfig, *config_->audioConfig);
            if (acfg) {
                OBSDataAutoRelease s = obs_data_create_from_json(acfg->encoderParams.dump().c_str());
                int mid = mixerId.value_or(acfg->mixerId);
                enc = obs_audio_encoder_create(acfg->encoderId.c_str(), AudioEncoderName(trackIdx).c_str(), s, mid, nullptr);
            } else {
                blog(LOG_ERROR, TAG "No audio encoder config found.");
                config_->audioConfig = OBS_STREAMING_ENC_PLACEHOLDER;
                return GetAudioEncoder();
            }
        }
        using_main_audio_encoder_ = false;
        return enc.Get();
    }

    bool PrepareOutputEncoders()
    {
        if (!output_) return false;
        ReleaseOutputEncoder();

        OBSEncoder venc = GetVideoEncoder();
        OBSEncoder aenc = GetAudioEncoder();

        std::vector<std::tuple<int, OBSEncoder>> additionalTracks;
        if (auto audioConfigId = config_->audioConfig) {
            auto acfg = FindById(GlobalMultiOutputConfig().audioConfig, *audioConfigId);
            if (acfg) {
                for (auto& track : acfg->audioTracks) {
                    OBSEncoder enc = GetAudioEncoder(track->output_track, track->mixer_track);
                    if (enc) additionalTracks.push_back({ track->output_track, enc });
                }
            }
        }

        if (!aenc || !venc) {
            ReleaseOutputEncoder();
            blog(LOG_ERROR, TAG "Failed to get encoders for kxtsune-ingest.");
            return false;
        }

        obs_output_set_audio_encoder(output_, obs_encoder_get_ref(aenc), 0);
        for (auto& [idx, enc] : additionalTracks)
            obs_output_set_audio_encoder(output_, obs_encoder_get_ref(enc), idx);
        obs_output_set_video_encoder(output_, obs_encoder_get_ref(venc));
        return true;
    }

    bool ReleaseOutputEncoder()
    {
        if (!output_) return true;
        if (obs_output_active(output_)) {
            blog(LOG_ERROR, TAG "Release encoder while output is active.");
            return false;
        }
        auto venc = obs_output_get_video_encoder(output_);
        if (venc) { obs_output_set_video_encoder(output_, nullptr); obs_encoder_release(venc); }
        auto aenc = obs_output_get_audio_encoder(output_, 0);
        if (aenc) { obs_output_set_audio_encoder(output_, nullptr, 0); obs_encoder_release(aenc); }
        return true;
    }

    bool ReleaseOutput()
    {
        if (output_) DisconnectSignals(output_);
        if (output_ && obs_output_active(output_)) obs_output_force_stop(output_);
        if (output_ && !obs_output_active(output_)) {
            ReleaseOutputService();
            ReleaseOutputEncoder();
            obs_output_release(output_);
            output_ = nullptr;
            ReleaseOutputSceneView();
        } else if (output_) {
            obs_output_release(output_);
            output_ = nullptr;
        }
        return true;
    }

    // ── streaming status update (timer tick) ─────────────────────────────────

    void UpdateStreamStatus()
    {
        if (!output_) return;

        static const char* units[] = { "bps","Kbps","Mbps","Gbps","Tbps","Pbps","Ebps","Zbps","Ybps" };
        using namespace std::chrono;

        auto new_bytes  = obs_output_get_total_bytes(output_);
        auto new_frames = obs_output_get_total_frames(output_);
        auto now = clock::now();
        auto interval = duration_cast<duration<double>>(now - last_info_time_).count();

        if (interval > 0) {
            auto dur = now - begin_time_;
            auto hh = duration_cast<hours>(dur);   dur -= hh;
            auto mm = duration_cast<minutes>(dur);  dur -= mm;
            auto ss = duration_cast<seconds>(dur);

            char strDur[64];
            snprintf(strDur, sizeof(strDur), "%02d:%02d:%02d", (int)hh.count(), (int)mm.count(), (int)ss.count());

            char strFps[32];
            snprintf(strFps, sizeof(strFps), "%d FPS",
                     (int)std::round((new_frames - total_frames_) / interval));

            auto bps = (new_bytes - total_bytes_) * 8.0 / interval;
            std::string strBps;
            if (bps > 0) {
                int maxUnit = (int)(sizeof(units)/sizeof(*units));
                int ui = std::min((int)(log10(bps) / 3), maxUnit - 1);
                auto s = std::to_string(bps / pow(1000, ui)).substr(0, 4);
                if (!s.empty() && s.back() == '.') s.pop_back();
                strBps = s + " " + units[ui];
            } else {
                strBps = "0 bps";
            }

            NotifyStatus(std::string("● Live — ") + strDur + "  " + strBps + "  " + strFps);
        }

        total_frames_ = new_frames;
        total_bytes_  = new_bytes;
        last_info_time_ = now;
    }

public:
    PushWidgetImpl(const std::string& targetid, QObject* parent = nullptr)
        : QObject(parent)
        , targetid_(targetid)
    {
        auto& global = GlobalMultiOutputConfig();
        config_ = FindById(global.targets, targetid_);
        if (!config_) return;

        timer_ = new QTimer(this);
        timer_->setInterval(1000);
        QObject::connect(timer_, &QTimer::timeout, [this]() {
            UpdateStreamStatus();
        });
    }

    ~PushWidgetImpl() override
    {
        if (timer_) timer_->stop();
        ReleaseOutput();
    }

    void SetStatusCallback(StatusCallback cb) override {
        statusCallback_ = std::move(cb);
    }

    bool IsRunning() override {
        return output_ != nullptr && obs_output_active(output_);
    }

    void StartStreaming() override
    {
        if (IsRunning()) return;
        if (!config_) return;

        // Guard: do not attempt to stream without a key
        if (config_->streamKey.empty()) {
            blog(LOG_INFO, TAG "StartStreaming skipped — no stream key set.");
            NotifyStatus("No detection key set.");
            return;
        }

        // Ensure serviceParam has the current key
        config_->serviceParam["key"] = config_->streamKey;

        ReleaseOutput();

        if (!output_) {
            OBSDataAutoRelease settings = obs_data_create_from_json(config_->outputParam.dump().c_str());
            auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
            if (!protocolInfo) {
                blog(LOG_ERROR, TAG "Invalid protocol \"%s\"", config_->protocol.c_str());
                return;
            }
            output_ = obs_output_create(protocolInfo->outputId, "multi-output", settings, nullptr);
            SetMeAsHandler(output_);
        }

        if (output_) {
            isUseDelay_ = false;
            auto profileConfig = obs_frontend_get_profile_config();
            if (profileConfig) {
                bool useDelay     = config_get_bool(profileConfig, "Output", "DelayEnable");
                bool preserveDly  = config_get_bool(profileConfig, "Output", "DelayPreserve");
                int  delaySec     = config_get_int (profileConfig, "Output", "DelaySec");
                obs_output_set_delay(output_,
                    useDelay ? delaySec : 0,
                    preserveDly ? OBS_OUTPUT_DELAY_PRESERVE : 0);
                if (useDelay && delaySec > 0) isUseDelay_ = true;
            }
        }

        if (!PrepareOutputService()) {
            NotifyStatus("Error: could not create RTMP service.");
            return;
        }
        if (!PrepareOutputEncoders()) {
            NotifyStatus("Error: could not create encoder.");
            return;
        }
        if (!PrepareEncoderSource()) {
            NotifyStatus("Error: scene not found.");
            return;
        }
        if (!obs_output_start(output_)) {
            NotifyStatus("Error: failed to start output.");
        }
    }

    void StopStreaming() override
    {
        if (!IsRunning()) return;
        obs_output_stop(output_);
    }

    void OnOBSEvent(obs_frontend_event ev) override
    {
        if (ev == OBS_FRONTEND_EVENT_EXIT
         || ev == OBS_FRONTEND_EVENT_PROFILE_CHANGED
         || ev == OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED)
        {
            if (IsRunning()) obs_output_force_stop(output_);
        }
        else if (ev == OBS_FRONTEND_EVENT_STREAMING_STARTING)
        {
            if (!IsRunning() && config_ && config_->syncStart && config_->enabled)
                StartStreaming();
        }
        else if (ev == OBS_FRONTEND_EVENT_STREAMING_STOPPING)
        {
            if (IsRunning() && config_ && config_->syncStop)
                StopStreaming();
        }
    }

    // ── IOBSOutputEventHandler ────────────────────────────────────────────────

    void OnStarting() override
    {
        GetGlobalService().RunInUIThread([this]() {
            begin_time_ = clock::now();
            NotifyStatus(obs_module_text("Status.Connecting"));
        });
    }

    void OnStarted() override
    {
        GetGlobalService().RunInUIThread([this]() {
            total_frames_ = 0; total_bytes_ = 0;
            last_info_time_ = clock::now();
            NotifyStatus(obs_module_text("Status.Streaming"));
            timer_->start();
        });
    }

    void OnReconnect() override
    {
        GetGlobalService().RunInUIThread([this]() {
            timer_->stop();
            NotifyStatus(obs_module_text("Status.Reconnecting"));
        });
    }

    void OnReconnected() override
    {
        GetGlobalService().RunInUIThread([this]() {
            total_frames_ = 0; total_bytes_ = 0;
            last_info_time_ = clock::now();
            NotifyStatus(obs_module_text("Status.Streaming"));
            timer_->start();
        });
    }

    void OnStopping() override
    {
        GetGlobalService().RunInUIThread([this]() {
            timer_->stop();
            NotifyStatus(obs_module_text("Status.Stopping"));
        });
    }

    void OnStopped(int code) override
    {
        GetGlobalService().RunInUIThread([this, code]() {
            timer_->stop();
            switch (code) {
            case  0:  NotifyStatus(""); break;
            case -1:  NotifyStatus(obs_module_text("Error.WrongRTMPUrl")); break;
            case -2:  NotifyStatus(obs_module_text("Error.ServerConnect")); break;
            case -3:  NotifyStatus(obs_module_text("Error.ServerHandshake")); break;
            case -4:  NotifyStatus(obs_module_text("Error.ServerRefuse")); break;
            default:  NotifyStatus(obs_module_text("Error.Unknown")); break;
            }
        });

        ReleaseOutputEncoder();
        ReleaseOutputSceneView();
    }
};

#include "push-widget.moc"

PushWidget* createPushWidget(const std::string& targetid, QObject* parent) {
    return new PushWidgetImpl(targetid, parent);
}
