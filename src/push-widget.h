#pragma once
#include "pch.h"
#include <functional>

// PushWidget is a pure abstract streaming-engine interface (no UI, no QObject inheritance).
// PushWidgetImpl (in push-widget.cpp) derives from QObject separately.
class PushWidget {
public:
    virtual ~PushWidget() {}

    // Start/stop the OBS output for the fixed kxtsune-ingest target.
    virtual void StartStreaming() = 0;
    virtual void StopStreaming() = 0;

    // Returns true if the output is currently active.
    virtual bool IsRunning() = 0;

    // Forward an OBS frontend event (sync-start / sync-stop etc.)
    virtual void OnOBSEvent(obs_frontend_event ev) = 0;

    // Status text callback — called on the UI thread whenever the status changes.
    // e.g. "Connecting…", "● Live — 00:01:23  2.4 Mbps  60 FPS", ""
    using StatusCallback = std::function<void(const std::string&)>;
    virtual void SetStatusCallback(StatusCallback cb) = 0;
};

PushWidget* createPushWidget(const std::string& targetId, QObject* parent = nullptr);
