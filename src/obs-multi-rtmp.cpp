#include "pch.h"

#include "push-widget.h"
#include "plugin-support.h"
#include "output-config.h"

#ifdef _WIN32
#include <Windows.h>
#endif

static class GlobalServiceImpl : public GlobalService
{
public:
    bool RunInUIThread(std::function<void()> task) override {
        if (uiThread_ == nullptr)
            return false;
        QMetaObject::invokeMethod(uiThread_, [func = std::move(task)]() {
            func();
        });
        return true;
    }

    QThread* uiThread_ = nullptr;
} s_service;

GlobalService& GetGlobalService() {
    return s_service;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Simplified dock: stream-key input + enable toggle + status label
// ─────────────────────────────────────────────────────────────────────────────

class MultiOutputWidget : public QWidget
{
public:
    MultiOutputWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setWindowTitle(obs_module_text("Title"));

        // ── stream key row ────────────────────────────────────────────────
        auto keyLabel = new QLabel(u8"Kxtsune Detection Key", this);

        keyEdit_ = new QLineEdit(this);
        keyEdit_->setPlaceholderText(u8"Enter your detection key…");
        keyEdit_->setEchoMode(QLineEdit::Password);

        auto updateKeyBtn = new QPushButton(u8"Update Key", this);

        auto howToLink = new QLabel(
            u8"<a href=\"https://www.kxtsune.com/creator/dashboard/auto-detect\">How to get it?</a>",
            this);
        howToLink->setTextFormat(Qt::RichText);
        howToLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
        howToLink->setOpenExternalLinks(true);

        auto keyRow = new QHBoxLayout();
        keyRow->addWidget(keyEdit_, 1);
        keyRow->addWidget(updateKeyBtn);
        keyRow->addWidget(howToLink);

        // ── enable/disable toggle button ──────────────────────────────────
        toggleBtn_ = new QPushButton(this);
        UpdateToggleButton(false);

        // ── status label ──────────────────────────────────────────────────
        statusLabel_ = new QLabel(u8"Not streaming", this);
        statusLabel_->setWordWrap(true);

        // ── layout ───────────────────────────────────────────────────────
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);
        layout->addWidget(keyLabel);
        layout->addLayout(keyRow);
        layout->addWidget(toggleBtn_);
        layout->addWidget(statusLabel_);
        layout->addStretch();
        setLayout(layout);

        // ── load config & wire up push widget ────────────────────────────
        LoadConfig();

        // Update Key button: save the key; restart output if already streaming
        QObject::connect(updateKeyBtn, &QPushButton::clicked, [this]() {
            auto key = keyEdit_->text().trimmed().toStdString();
            if (key.empty()) {
                QMessageBox::warning(this,
                    u8"Kxtsune Auto Detect",
                    u8"Please enter your Kxtsune Detection Key before saving.");
                return;
            }
            auto& global = GlobalMultiOutputConfig();
            if (!global.targets.empty()) {
                auto& target = *global.targets.front();
                target.streamKey = key;
                target.serviceParam["key"] = key;
            }
            UpdateToggleButton(GetTargetEnabled());
            SaveConfig();

            // If currently streaming, restart so the new key takes effect
            if (pushWidget_ && pushWidget_->IsRunning()) {
                pushWidget_->StopStreaming();
                pushWidget_->StartStreaming();
            }
        });

        // Toggle enable/disable
        QObject::connect(toggleBtn_, &QPushButton::clicked, [this]() {
            if (!pushWidget_) return;
            bool nowEnabled = !GetTargetEnabled();

            // Guard: require a key before enabling.
            // Accept the key currently typed in the field even if not yet saved.
            if (nowEnabled) {
                auto typedKey = keyEdit_->text().trimmed().toStdString();
                auto& global = GlobalMultiOutputConfig();
                bool hasKey = !typedKey.empty();
                if (!hasKey && !global.targets.empty())
                    hasKey = !global.targets.front()->streamKey.empty();

                if (!hasKey) {
                    QMessageBox::warning(this,
                        u8"Kxtsune Auto Detect",
                        u8"Please enter your Kxtsune Detection Key before enabling.");
                    return;
                }

                // Auto-save the typed key so it takes effect immediately
                if (!typedKey.empty() && !global.targets.empty()) {
                    auto& target = *global.targets.front();
                    target.streamKey = typedKey;
                    target.serviceParam["key"] = typedKey;
                }
            }

            SetTargetEnabled(nowEnabled);
            UpdateToggleButton(nowEnabled);
            if (nowEnabled) {
                // If OBS is already streaming, start immediately
                if (obs_frontend_streaming_active()) {
                    pushWidget_->StartStreaming();
                }
            } else {
                if (pushWidget_->IsRunning()) {
                    pushWidget_->StopStreaming();
                }
                statusLabel_->setText(u8"Not streaming");
            }
            SaveConfig();
        });
    }

    // Called from the OBS frontend event callback
    void OnOBSEvent(obs_frontend_event ev) {
        if (pushWidget_)
            pushWidget_->OnOBSEvent(ev);

        if (ev == OBS_FRONTEND_EVENT_EXIT || ev == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
            SaveConfig();
            if (ev == OBS_FRONTEND_EVENT_PROFILE_CHANGED)
                LoadConfig();
        }
    }

    void SaveConfig() {
        SaveMultiOutputConfig();
    }

    void LoadConfig() {
        // Rebuild the fixed target from disk
        LoadMultiOutputConfig();

        // Re-create the push widget for the single target
        if (pushWidget_) {
            delete pushWidget_;
            pushWidget_ = nullptr;
        }

        auto& global = GlobalMultiOutputConfig();
        if (!global.targets.empty()) {
            auto& target = *global.targets.front();
            pushWidget_ = createPushWidget(target.id, this);
            pushWidget_->SetStatusCallback([this](const std::string& msg) {
                statusLabel_->setText(QString::fromUtf8(msg));
            });

            // Populate UI from loaded config
            keyEdit_->setText(QString::fromUtf8(target.streamKey));
            bool enabled = target.enabled;
            UpdateToggleButton(enabled);
            statusLabel_->setText(enabled ? u8"Not streaming" : u8"Disabled");
        }
    }

private:
    QLineEdit*   keyEdit_     = nullptr;
    QPushButton* toggleBtn_   = nullptr;
    QLabel*      statusLabel_ = nullptr;
    PushWidget*  pushWidget_  = nullptr;

    bool GetTargetEnabled() const {
        auto& global = GlobalMultiOutputConfig();
        if (global.targets.empty()) return false;
        return global.targets.front()->enabled;
    }

    void SetTargetEnabled(bool enabled) {
        auto& global = GlobalMultiOutputConfig();
        if (!global.targets.empty())
            global.targets.front()->enabled = enabled;
    }

    void ApplyStreamKey() {
        auto key = keyEdit_->text().trimmed().toStdString();
        auto& global = GlobalMultiOutputConfig();
        if (!global.targets.empty()) {
            auto& target = *global.targets.front();
            target.streamKey = key;
            target.serviceParam["key"] = key;
        }
        // Re-enable button state based on whether we now have a key
        bool enabled = GetTargetEnabled();
        UpdateToggleButton(enabled);
        SaveConfig();
    }

    void UpdateToggleButton(bool enabled) {
        if (enabled) {
            toggleBtn_->setText(u8"Disable Kxtsune Auto Detect");
            toggleBtn_->setStyleSheet("color: #ff6b6b;");
        } else {
            toggleBtn_->setText(u8"Enable Kxtsune Auto Detect");
            toggleBtn_->setStyleSheet("");
        }
        toggleBtn_->setEnabled(true);
    }
};


OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("kxtsune-obs", "en-US")
OBS_MODULE_AUTHOR("kxtsune")

bool obs_module_load()
{
    auto mainwin = (QMainWindow*)obs_frontend_get_main_window();
    if (mainwin == nullptr)
        return false;
    QMetaObject::invokeMethod(mainwin, []() {
        s_service.uiThread_ = QThread::currentThread();
    });

    auto dock = new MultiOutputWidget();
    dock->setObjectName("kxtsune-obs-dock");
    if (!obs_frontend_add_dock_by_id("kxtsune-obs-dock", obs_module_text("Title"), dock)) {
        delete dock;
        return false;
    }

    blog(LOG_INFO, TAG "version: %s", PLUGIN_VERSION);

    obs_frontend_add_event_callback(
        [](obs_frontend_event event, void* private_data) {
            auto dock = static_cast<MultiOutputWidget*>(private_data);
            dock->OnOBSEvent(event);
        },
        dock
    );

    return true;
}

const char* obs_module_description(void)
{
    return "Kxtsune Streaming Plugin";
}
