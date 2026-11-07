#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QString>

namespace gofi {

class AppController;
class SettingsManager;
class TaskTimer;

class SystemHookService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool dndFocusActive READ isDndFocusActive NOTIFY dndFocusActiveChanged)
    Q_PROPERTY(bool sleepInhibitActive READ isSleepInhibitActive NOTIFY sleepInhibitActiveChanged)
    Q_PROPERTY(QString hooksDirectory READ hooksDirectory CONSTANT)

public:
    explicit SystemHookService(AppController* controller,
                               SettingsManager* settings,
                               TaskTimer* timer,
                               QObject* parent = nullptr);
    ~SystemHookService() override;

    [[nodiscard]] bool isDndFocusActive() const noexcept { return m_dndActive; }
    [[nodiscard]] bool isSleepInhibitActive() const noexcept { return m_sleepInhibitActive; }
    [[nodiscard]] QString hooksDirectory() const;
    void setCustomHooksDirectory(const QString& path);

    Q_INVOKABLE void ensureHooksDirectory();
    Q_INVOKABLE bool testHook(const QString& eventName);
    Q_INVOKABLE void openHooksFolder();

    void triggerHook(const QString& eventName,
                     const QString& taskTitle = QString(),
                     const QString& taskKey = QString(),
                     const QString& taskId = QString());

public slots:
    void onWorkStarted();
    void onBreakStarted();
    void onPomodoroCompleted();
    void onTimerStopped();
    void onTaskCompleted(const QString& taskTitle,
                        const QString& taskKey = QString(),
                        const QString& taskId = QString());

signals:
    void dndFocusActiveChanged();
    void sleepInhibitActiveChanged();
    void hookExecuted(const QString& eventName, bool success, const QString& output);

private:
    void applyDnd(bool enable);
    void applySleepInhibit(bool enable);
    void runCommandOrScript(const QString& eventName,
                            const QString& customCommand,
                            const QString& scriptBaseName,
                            const QProcessEnvironment& env);

    AppController* m_controller = nullptr;
    SettingsManager* m_settings = nullptr;
    TaskTimer* m_timer = nullptr;

    bool m_dndActive = false;
    bool m_sleepInhibitActive = false;
    QString m_customHooksDirectory;
    uint m_dndCookie = 0;
    uint m_sleepCookie = 0;
    bool m_gnomeBannersDisabled = false;
};

} // namespace gofi
