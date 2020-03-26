#include "obs-multi-rtmp.h"
#include "obs-module.h"
#include "obs-frontend-api.h"
#include "util/config-file.h"

#include <QMainWindow>
#include <QWidget>
#include <QFrame>
#include <QDockWidget>
#include <QLabel>
#include <QString>
#include <QPushButton>
#include <QScrollArea>
#include <QGridLayout>
#include <QEvent>
#include <QThread>
#include <QLineEdit>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>

#include <list>
#include <memory>

#include <windows.h>


#define ConfigSection "obs-multi-rtmp"



class IOBSOutputEventHanlder
{
public:
    virtual void OnStarting() {}
    static void OnOutputStarting(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarting();
    }

    virtual void OnStarted() {}
    static void OnOutputStarted(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarted();
    }

    virtual void OnStopping() {}
    static void OnOutputStopping(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopping();
    }

    virtual void OnStopped(int code) {}
    static void OnOutputStopped(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopped(calldata_int(param, "code"));
    }

    virtual void OnReconnect() {}
    static void OnOutputReconnect(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnect();
    }

    virtual void OnReconnected() {}
    static void OnOutputReconnected(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnected();
    }

    virtual void onDeactive() {}
    static void OnOutputDeactive(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->onDeactive();
    }

    void SetAsHandler(obs_output_t* output)
    {
        auto outputSignal = obs_output_get_signal_handler(output);
        if (outputSignal)
        {
            signal_handler_connect(outputSignal, "starting", &IOBSOutputEventHanlder::OnOutputStarting, this);
            signal_handler_connect(outputSignal, "start", &IOBSOutputEventHanlder::OnOutputStarted, this);
            signal_handler_connect(outputSignal, "reconnect", &IOBSOutputEventHanlder::OnOutputReconnect, this);
            signal_handler_connect(outputSignal, "reconnect_success", &IOBSOutputEventHanlder::OnOutputReconnected, this);
            signal_handler_connect(outputSignal, "stopping", &IOBSOutputEventHanlder::OnOutputStopping, this);
            signal_handler_connect(outputSignal, "deactivate", &IOBSOutputEventHanlder::OnOutputDeactive, this);
            signal_handler_connect(outputSignal, "stop", &IOBSOutputEventHanlder::OnOutputStopped, this);
        }
    }
};



template<class T>
bool RunInUIThread(T&& func)
{
    auto mainwnd = (QMainWindow*)obs_frontend_get_main_window();
    if (mainwnd == nullptr)
        return false;
    QMetaObject::invokeMethod(mainwnd, [func = std::move(func)]() {
        func();
    });
    return true;
}



class EditOutputWidget : public QDialog
{
    QLineEdit* name_ = 0;
    QLineEdit* rtmp_path_ = 0;
    QLineEdit* rtmp_key_ = 0;

public:
    EditOutputWidget()
    {
        setWindowTitle(u8"配信設定");

        auto layout = new QGridLayout(this);
        {
            auto linelayout = new QHBoxLayout(this);
            linelayout->addWidget(new QLabel(u8"名前", this));
            linelayout->addWidget(name_ = new QLineEdit("", this), 1);
            layout->addLayout(linelayout, 0, 0);
        }

        {
            auto linelayout = new QHBoxLayout(this);
            linelayout->addWidget(new QLabel(u8"RTMP サバ", this));
            linelayout->addWidget(rtmp_path_ = new QLineEdit("", this), 1);
            layout->addLayout(linelayout, 1, 0);
        }

        {
            auto linelayout = new QHBoxLayout(this);
            linelayout->addWidget(new QLabel(u8"RTMP キー", this));
            linelayout->addWidget(rtmp_key_ = new QLineEdit(u8"", this), 1);
            layout->addLayout(linelayout, 2, 0);
        }

        auto okbtn = new QPushButton(u8"OK", this);
        QObject::connect(okbtn, &QPushButton::clicked, [this]() {
            done(DialogCode::Accepted);
        });
        layout->addWidget(okbtn, 3, 0);

        resize(540, 160);
        setLayout(layout);
    }

    void setName(QString name) { name_->setText(name); }
    QString getName() { return name_->text(); }

    void setPath(QString path) { rtmp_path_->setText(path); }
    QString getPath() { return rtmp_path_->text(); }

    void setKey(QString key) { rtmp_key_->setText(key); }
    QString getKey() { return rtmp_key_->text(); }
};



class PushWidget : public QWidget, public IOBSOutputEventHanlder
{
    QPushButton* btn_ = 0;
    QLabel* name_ = 0;
    QLabel* fps_ = 0;
    QLabel* msg_ = 0;

    using clock = std::chrono::steady_clock;
    clock::time_point last_info_time_;
    uint64_t total_frames_ = 0;
    QTimer* timer_ = 0;

    QPushButton* edit_btn_ = 0;
    QPushButton* remove_btn_ = 0;

    std::string rtmp_path_;
    std::string rtmp_key_;

    obs_service_t* service_ = 0;
    obs_output_t* output_ = 0;

public:
    PushWidget(QWidget* parent = 0)
        : QWidget(parent)
    {
        QObject::setObjectName("push-widget");

        timer_ = new QTimer(this);
        timer_->setInterval(std::chrono::milliseconds(1000));
        QObject::connect(timer_, &QTimer::timeout, [this]() {
            auto new_frames = obs_output_get_total_frames(output_);
            auto now = clock::now();

            auto intervalms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_info_time_).count();
            if (intervalms > 0)
            {
                auto text = std::to_string((new_frames - total_frames_) * 1000 / intervalms) + " FPS";
                fps_->setText(text.c_str());
            }

            total_frames_ = new_frames;
            last_info_time_ = now;
        });

        auto layout = new QGridLayout(this);
        layout->addWidget(name_ = new QLabel(u8"新規配信", this), 0, 0, 1, 2);
        layout->addWidget(fps_ = new QLabel(u8"", this), 0, 2);
        layout->addWidget(btn_ = new QPushButton(u8"開始", this), 1, 0);
        QObject::connect(btn_, &QPushButton::clicked, [this]() {
            StartStop();
        });

        layout->addWidget(edit_btn_ = new QPushButton(u8"編集", this), 1, 1);
        QObject::connect(edit_btn_, &QPushButton::clicked, [this]() {
            ShowEditDlg();
        });

        layout->addWidget(remove_btn_ = new QPushButton(u8"削除", this), 1, 2);
        QObject::connect(remove_btn_, &QPushButton::clicked, [this]() {
            auto msgbox = new QMessageBox(QMessageBox::Icon::Question,
                u8"質問",
                u8"削除してもいいですか",
                QMessageBox::Yes | QMessageBox::No,
                this
            );
            if (msgbox->exec() == QMessageBox::Yes)
                delete this;
        });

        layout->addWidget(msg_ = new QLabel(u8"", this), 2, 0, 1, 2);
        layout->addItem(new QSpacerItem(0, 10), 3, 0);
        setLayout(layout);
    }

    ~PushWidget()
    {
        if (output_ != nullptr)
        {
            obs_output_release(output_);
        }
        if (service_)
        {
            obs_service_release(service_);
        }
    }

    void ResetInfo()
    {
        total_frames_ = 0;
        last_info_time_ = clock::now();
        fps_->setText("");
    }

    void StartStop()
    {
        if (output_ == 0)
        {
            output_ = obs_output_create("rtmp_output", "multi-output", nullptr, nullptr);
            service_ = obs_service_create("rtmp_custom", "multi-output-service", nullptr, nullptr);
            obs_output_set_service(output_, service_);

            SetAsHandler(output_);
        }

        if (output_ != 0 && obs_output_active(output_) == false)
        {
            obs_output_t* stream_out = obs_frontend_get_streaming_output();
            if (stream_out)
            {
                auto venc = obs_output_get_video_encoder(stream_out);
                auto aenc = obs_output_get_audio_encoder(stream_out, 0);
                obs_output_set_video_encoder(output_, venc);
                obs_output_set_audio_encoder(output_, aenc, 0);
                obs_output_release(stream_out);
            }
            else
            {
                auto msgbox = new QMessageBox(QMessageBox::Icon::Critical, 
                    u8"お知らせ", 
                    u8"OBS本体の「配信開始」一回使ってください（すぐに停止してもいい）",
                    QMessageBox::StandardButton::Ok,
                    this
                    );
                msgbox->exec();
                return;
            }

            auto conf = obs_service_get_settings(service_);
            obs_data_set_string(conf, "server", rtmp_path_.c_str());
            obs_data_set_string(conf, "key", rtmp_key_.c_str());
            obs_service_update(service_, conf);
            obs_data_release(conf);

            obs_output_start(output_);
        }
        else
        {
            obs_output_force_stop(output_);
        }
    }

    bool ShowEditDlg()
    {
        auto dlg = new EditOutputWidget();
        dlg->setName(name_->text());
        dlg->setPath(rtmp_path_.c_str());
        dlg->setKey(rtmp_key_.c_str());

        if (dlg->exec() == QDialog::DialogCode::Accepted)
        {
            name_->setText(dlg->getName());
            rtmp_path_ = dlg->getPath().toUtf8();
            rtmp_key_ = dlg->getKey().toUtf8();
            return true;
        }
        else
            return false;
    }

    QJsonObject Serialize()
    {
        QJsonObject ret;
        ret.insert("name", name_->text());
        ret.insert("rtmp-path", QString::fromUtf8(rtmp_path_.c_str()));
        ret.insert("rtmp-key", QString::fromUtf8(rtmp_key_.c_str()));
        return ret;
    }

    static PushWidget* Deserialize(QJsonObject json, QWidget* parent)
    {
        auto it_name = json.find("name");
        auto it_path = json.find("rtmp-path");
        auto it_key = json.find("rtmp-key");
        if (it_name != json.end() && it_name->isString()
            && it_path != json.end() && it_path->isString()
            && it_key != json.end() && it_key->isString()
        ) {
            auto ret = new PushWidget(parent);
            ret->name_->setText(it_name->toString());
            ret->rtmp_path_ = it_path->toString().toUtf8();
            ret->rtmp_key_ = it_key->toString().toUtf8();
            return ret;
        }
        else
            return nullptr;
    }

    void SetMsg(QString msg)
    {
        msg_->setText(msg);
        msg_->setToolTip(msg);
    }

    // obs logical
    void OnStarting() override
    {
        RunInUIThread([this]()
        {
            remove_btn_->setEnabled(false);
            btn_->setEnabled(false);
            SetMsg(u8"接続中");
            remove_btn_->setEnabled(false);
        });
    }

    void OnStarted() override
    {
        RunInUIThread([this]() {
            remove_btn_->setEnabled(false);
            btn_->setText(u8"停止");
            btn_->setEnabled(true);
            SetMsg(u8"配信中");

            ResetInfo();
            timer_->start();
        });
    }

    void OnReconnect() override
    {
        RunInUIThread([this]() {
            remove_btn_->setEnabled(false);
            btn_->setText(u8"停止");
            btn_->setEnabled(true);
            SetMsg(u8"再接続中");
        });
    }

    void OnReconnected() override
    {
        RunInUIThread([this]() {
            remove_btn_->setEnabled(false);
            btn_->setText(u8"停止");
            btn_->setEnabled(true);
            SetMsg(u8"配信中");

            ResetInfo();
        });
    }

    void OnStopping() override
    {
        RunInUIThread([this]() {
            remove_btn_->setEnabled(false);
            btn_->setText(u8"停止");
            btn_->setEnabled(false);
            SetMsg(u8"停止中...");
        });
    }

    void OnStopped(int code) override
    {
        RunInUIThread([this, code]() {
            ResetInfo();
            timer_->stop();

            remove_btn_->setEnabled(true);
            btn_->setText(u8"開始");
            btn_->setEnabled(true);
            SetMsg(u8"");

            switch(code)
            {
                case 0:
                    SetMsg(u8"");
                    break;
                case -1:
                    SetMsg(u8"！RTMPアドレスエラーです");
                    break;
                case -2:
                    SetMsg(u8"！サーバーに接続失敗");
                    break;
                case -3:
                    SetMsg(u8"！サーバーに通信失敗");
                    break;
                case -4:
                    SetMsg(u8"！サーバーに拒否された");
                    break;
                default:
                    SetMsg(u8"！知らんエラー");
                    break;
            }
        });
    }
};



class MultiOutputWidget : public QDockWidget
{
public:
    MultiOutputWidget(QWidget* parent = 0)
        : QDockWidget(parent)
    {
        setWindowTitle(u8"同時配信");
        setFeatures((DockWidgetFeatures)(AllDockWidgetFeatures & ~DockWidgetClosable));

        // save dock location
        QObject::connect(this, &QDockWidget::dockLocationChanged, [](Qt::DockWidgetArea area) {
            config_set_int(obs_frontend_get_global_config(), "obs-multi-rtmp", "DockLocation", (int)area);
        });

        scroll_ = new QScrollArea(this);
        scroll_->move(0, 22);

        container_ = new QWidget(this);
        layout_ = new QGridLayout(container_);
        layout_->setAlignment(Qt::AlignmentFlag::AlignTop);

        // init widget
        addButton_ = new QPushButton(u8"新規配信登録", container_);
        QObject::connect(addButton_, &QPushButton::clicked, [this]() {
            auto pushwidget = new PushWidget(container_);
            layout_->addWidget(pushwidget);
            if (!pushwidget->ShowEditDlg())
                delete pushwidget;
        });
        layout_->addWidget(addButton_);

        // load config
        LoadConfig();

        scroll_->setWidgetResizable(true);
        scroll_->setWidget(container_);

        setLayout(layout_);

        resize(200, 400);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Resize)
        {
            scroll_->resize(width(), height() - 22);
        }
        return QDockWidget::event(event);
    }

    void SaveConfig()
    {
        QJsonArray targetlist;
        for(auto& c : container_->children())
        {
            if (c->objectName() == "push-widget")
            {
                auto w = static_cast<PushWidget*>(c);
                targetlist.append(w->Serialize());
            }
        }
        QJsonDocument jsondoc(targetlist);
        config_set_string(obs_frontend_get_global_config(), ConfigSection, "targets", jsondoc.toBinaryData().toBase64());
    }

    void LoadConfig()
    {
        auto base64str = config_get_string(obs_frontend_get_global_config(), ConfigSection, "targets");
        if (base64str && *base64str)
        {
            auto bindat = QByteArray::fromBase64(base64str);
            auto json = QJsonDocument::fromBinaryData(bindat);
            if (json.isArray())
            {
                for(auto x : json.array())
                {
                    if (x.isObject())
                    {
                        auto pushwidget = PushWidget::Deserialize(((QJsonValue)x).toObject(), container_);
                        if (pushwidget)
                            layout_->addWidget(pushwidget);
                    }
                }
            }
        }
    }

private:
    QWidget* container_ = 0;
    QScrollArea* scroll_ = 0;
    QGridLayout* layout_ = 0;
    QPushButton* addButton_ = 0;
};



OBS_DECLARE_MODULE()

bool obs_module_load()
{
    auto mainwin = (QMainWindow*)obs_frontend_get_main_window();
    auto dock = new MultiOutputWidget(mainwin);
    auto docklocation = config_get_int(obs_frontend_get_global_config(), ConfigSection, "DockLocation");
    //obs_frontend_add_dock(dock);
    mainwin->addDockWidget((Qt::DockWidgetArea)docklocation, dock);
    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *private_data) {
            if (event == obs_frontend_event::OBS_FRONTEND_EVENT_EXIT)
            {
                static_cast<MultiOutputWidget*>(private_data)->SaveConfig();
            }
        }, dock
    );

    return true;
}
