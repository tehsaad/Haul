#pragma once

#include "storage/SettingsRepository.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace haul::ui {

// Preferences. Values are validated by SettingsRepository and only saved when valid;
// saving immediately changes how the download engine behaves.
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(SettingsRepository *settings, QWidget *parent = nullptr);

    void load(const AppSettings &settings);
    AppSettings collect() const;

private:
    void save();
    void restoreDefaults();

    SettingsRepository *m_settings;
    QLineEdit *m_directory;
    QSpinBox *m_maxConcurrent;
    QSpinBox *m_maxConnections;
    QSpinBox *m_speedLimit;
    QCheckBox *m_autoRetry;
    QSpinBox *m_maxRetries;
    QSpinBox *m_retryDelay;
    QComboBox *m_defaultAction;
    QComboBox *m_theme;
    QCheckBox *m_confirmDelete;
    QCheckBox *m_startQueueOnLaunch;
    QCheckBox *m_browser = nullptr;
    QCheckBox *m_launchAtLogin = nullptr;
    QLabel *m_status;
};

} // namespace haul::ui
