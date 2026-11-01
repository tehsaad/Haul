#include "AddDownloadDialog.h"

#include "Theme.h"
#include "Widgets.h"
#include "app/DownloadManager.h"
#include "core/Checksum.h"
#include "core/FileNameUtils.h"
#include "core/Formatters.h"
#include "core/UrlValidator.h"
#include "storage/SettingsRepository.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace haul::ui {

AddDownloadDialog::AddDownloadDialog(DownloadManager *manager, SettingsRepository *settings, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("Add download"));
    setMinimumWidth(500);
    setModal(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    // URL
    root->addWidget(makeLabel(QStringLiteral("URL"), QStringLiteral("caption"), this));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(QStringLiteral("https://example.com/file.zip"));
    m_url->addAction(Theme::icon(QStringLiteral("link")), QLineEdit::LeadingPosition);
    m_url->setClearButtonEnabled(true);
    root->addWidget(m_url);
    m_urlError = makeLabel(QString(), QStringLiteral("error"), this);
    m_urlError->setWordWrap(true);
    m_urlError->hide();
    root->addWidget(m_urlError);

    // Inspection result card
    auto *infoCard = makeCard(this);
    auto *info = new QGridLayout(infoCard);
    info->setContentsMargins(14, 12, 14, 12);
    info->setHorizontalSpacing(18);
    info->setVerticalSpacing(6);
    auto *statusRow = new QHBoxLayout;
    m_inspectStatus = makeLabel(QStringLiteral("Paste a link above."), QStringLiteral("muted"), infoCard);
    m_inspectStatus->setWordWrap(true);
    m_cancelInspect = makeButton(QStringLiteral("Stop"), QString(), QStringLiteral("ghost"), infoCard);
    m_cancelInspect->hide();
    statusRow->addWidget(m_inspectStatus, 1);
    statusRow->addWidget(m_cancelInspect);
    info->addLayout(statusRow, 0, 0, 1, 2);
    m_sizeValue = makeLabel(QStringLiteral("—"), QStringLiteral("value"), infoCard);
    m_resumeValue = makeLabel(QStringLiteral("—"), QStringLiteral("value"), infoCard);
    m_typeValue = makeLabel(QStringLiteral("—"), QStringLiteral("value"), infoCard);
    info->addWidget(makeLabel(QStringLiteral("Size"), QStringLiteral("muted"), infoCard), 1, 0);
    info->addWidget(m_sizeValue, 1, 1);
    info->addWidget(makeLabel(QStringLiteral("Resume"), QStringLiteral("muted"), infoCard), 2, 0);
    info->addWidget(m_resumeValue, 2, 1);
    info->addWidget(makeLabel(QStringLiteral("Type"), QStringLiteral("muted"), infoCard), 3, 0);
    info->addWidget(m_typeValue, 3, 1);
    info->setColumnStretch(1, 1);
    root->addWidget(infoCard);

    // Destination
    auto *form = new QGridLayout;
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->addWidget(makeLabel(QStringLiteral("Save to"), QStringLiteral("caption"), this), 0, 0, 1, 2);
    m_directory = new QLineEdit(m_settings->current().downloadDirectory, this);
    auto *browse = makeButton(QStringLiteral("Browse…"), QStringLiteral("folder"), QString(), this);
    form->addWidget(m_directory, 1, 0);
    form->addWidget(browse, 1, 1);

    form->addWidget(makeLabel(QStringLiteral("File name (optional)"), QStringLiteral("caption"), this), 2, 0, 1, 2);
    m_fileName = new QLineEdit(this);
    m_fileName->setPlaceholderText(QStringLiteral("Detected from the server"));
    form->addWidget(m_fileName, 3, 0, 1, 2);
    m_conflictHint = makeLabel(QString(), QStringLiteral("warning"), this);
    m_conflictHint->setWordWrap(true);
    m_conflictHint->hide();
    form->addWidget(m_conflictHint, 4, 0, 1, 2);

    form->addWidget(makeLabel(QStringLiteral("Priority"), QStringLiteral("caption"), this), 5, 0);
    form->addWidget(makeLabel(QStringLiteral("SHA-256 (optional)"), QStringLiteral("caption"), this), 5, 1);
    m_priority = new QComboBox(this);
    m_priority->addItem(QStringLiteral("Low"), static_cast<int>(Priority::Low));
    m_priority->addItem(QStringLiteral("Normal"), static_cast<int>(Priority::Normal));
    m_priority->addItem(QStringLiteral("High"), static_cast<int>(Priority::High));
    m_priority->setCurrentIndex(1);
    m_checksum = new QLineEdit(this);
    m_checksum->setPlaceholderText(QStringLiteral("Paste a checksum from a trusted source"));
    form->addWidget(m_priority, 6, 0);
    form->addWidget(m_checksum, 6, 1);
    form->setColumnStretch(0, 1);
    form->setColumnStretch(1, 1);
    root->addLayout(form);

    // Login, tucked away until it is needed
    m_loginToggle = new QToolButton(this);
    m_loginToggle->setObjectName(QStringLiteral("Disclosure"));
    m_loginToggle->setText(QStringLiteral("Login"));
    m_loginToggle->setCheckable(true);
    m_loginToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_loginToggle->setArrowType(Qt::RightArrow);
    root->addWidget(m_loginToggle, 0, Qt::AlignLeft);
    m_loginBox = new QWidget(this);
    auto *login = new QGridLayout(m_loginBox);
    login->setContentsMargins(0, 0, 0, 0);
    login->setHorizontalSpacing(10);
    login->setVerticalSpacing(4);
    m_username = new QLineEdit(m_loginBox);
    m_username->setPlaceholderText(QStringLiteral("User name"));
    m_password = new QLineEdit(m_loginBox);
    m_password->setPlaceholderText(QStringLiteral("Password"));
    m_password->setEchoMode(QLineEdit::Password);
    m_cookie = new QLineEdit(m_loginBox);
    m_cookie->setPlaceholderText(QStringLiteral("Cookie, if the site needs one: name=value"));
    login->addWidget(m_username, 0, 0);
    login->addWidget(m_password, 0, 1);
    login->addWidget(m_cookie, 1, 0, 1, 2);
    m_loginNote = makeLabel(QStringLiteral("Not saved. Haul forgets this when it closes."), QStringLiteral("muted"), m_loginBox);
    m_loginNote->setWordWrap(true);
    login->addWidget(m_loginNote, 2, 0, 1, 2);
    m_loginBox->hide();
    root->addWidget(m_loginBox);

    m_formError = makeLabel(QString(), QStringLiteral("error"), this);
    m_formError->setWordWrap(true);
    m_formError->hide();
    root->addWidget(m_formError);

    // Buttons
    auto *buttons = new QHBoxLayout;
    auto *cancel = makeButton(QStringLiteral("Cancel"), QString(), QStringLiteral("ghost"), this);
    m_queueButton = makeButton(QStringLiteral("Add to queue"), QStringLiteral("queue"), QString(), this);
    m_startButton = makeButton(QStringLiteral("Start now"), QStringLiteral("download"), QStringLiteral("primary"), this);
    buttons->addWidget(cancel);
    buttons->addStretch();
    buttons->addWidget(m_queueButton);
    buttons->addWidget(m_startButton);
    root->addLayout(buttons);

    if (m_settings->current().startImmediately)
        m_startButton->setDefault(true);
    else
        m_queueButton->setDefault(true);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(450);

    connect(m_loginToggle, &QToolButton::toggled, this, [this](bool open) {
        m_loginToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        m_loginBox->setVisible(open);
    });
    for (QLineEdit *field : { m_username, m_password, m_cookie })
        connect(field, &QLineEdit::textEdited, this, &AddDownloadDialog::onUrlEdited);
    connect(m_url, &QLineEdit::textChanged, this, &AddDownloadDialog::onUrlEdited);
    connect(&m_debounce, &QTimer::timeout, this, &AddDownloadDialog::startInspection);
    connect(m_cancelInspect, &QPushButton::clicked, this, &AddDownloadDialog::cancelInspection);
    connect(&m_watcher, &QFutureWatcher<ProbeResult>::finished, this, &AddDownloadDialog::onInspectionFinished);
    connect(browse, &QPushButton::clicked, this, &AddDownloadDialog::browseDirectory);
    connect(m_directory, &QLineEdit::textChanged, this, &AddDownloadDialog::updateConflictHint);
    connect(m_fileName, &QLineEdit::textEdited, this, [this] {
        m_nameEditedByUser = true;
        updateConflictHint();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_queueButton, &QPushButton::clicked, this, [this] { submit(false); });
    connect(m_startButton, &QPushButton::clicked, this, [this] { submit(true); });

    // Convenience: if the clipboard holds a download link, offer it.
    const QString clip = QApplication::clipboard()->text().trimmed();
    if (validateDownloadUrl(clip).ok)
        m_url->setText(clip);
    updateButtons();
}

AddDownloadDialog::~AddDownloadDialog()
{
    // The probe thread may still be running: tell it to stop. It only touches its own data.
    if (m_probeCancel)
        m_probeCancel->store(true);
}

void AddDownloadDialog::setUrl(const QString &url)
{
    m_url->setText(url);
}

void AddDownloadDialog::setFileName(const QString &name)
{
    m_fileName->setText(sanitizeFileName(name));
    m_nameEditedByUser = true;
    updateConflictHint();
}

void AddDownloadDialog::setReferrer(const QString &referrer)
{
    m_referrer = referrer;
    if (!m_referrer.isEmpty() && validateDownloadUrl(m_url->text()).ok)
        m_debounce.start(); // check again the way the site expects to be asked
}

DownloadLogin AddDownloadDialog::login() const
{
    DownloadLogin value;
    value.referer = m_referrer.toUtf8();
    value.username = m_username->text();
    value.password = m_password->text();
    value.cookie = m_cookie->text().trimmed().toUtf8();
    return value;
}

void AddDownloadDialog::onUrlEdited()
{
    const QString scheme = QUrl(m_url->text().trimmed()).scheme().toLower();
    const bool plainText = scheme == QLatin1String("http") || scheme == QLatin1String("ftp");
    const bool hasLogin = !m_username->text().isEmpty() || !m_password->text().isEmpty() || !m_cookie->text().isEmpty();
    m_loginNote->setText(plainText && hasLogin
                             ? QStringLiteral("This link isn't encrypted, so anyone on the network could read this login. Not saved.")
                             : QStringLiteral("Not saved. Haul forgets this when it closes."));
    setRole(m_loginNote, plainText && hasLogin ? QStringLiteral("warning") : QStringLiteral("muted"));

    cancelInspection();
    m_probeFailedPermanently = false;
    m_sizeValue->setText(QStringLiteral("—"));
    m_resumeValue->setText(QStringLiteral("—"));
    m_typeValue->setText(QStringLiteral("—"));

    const QString text = m_url->text().trimmed();
    if (text.isEmpty()) {
        m_urlError->hide();
        m_url->setProperty("invalid", false);
        m_inspectStatus->setText(QStringLiteral("Paste a link above."));
    } else {
        const UrlCheck check = validateDownloadUrl(text);
        m_url->setProperty("invalid", !check.ok);
        m_urlError->setVisible(!check.ok);
        m_urlError->setText(check.error);
        if (check.ok) {
            m_inspectStatus->setText(QStringLiteral("Checking…"));
            m_debounce.start();
        } else {
            m_inspectStatus->setText(QStringLiteral("Fix the URL to continue."));
        }
        if (!m_nameEditedByUser && check.ok)
            m_fileName->setText(fileNameFromUrl(check.url));
    }
    m_url->style()->unpolish(m_url);
    m_url->style()->polish(m_url);
    updateConflictHint();
    updateButtons();
}

void AddDownloadDialog::startInspection()
{
    const UrlCheck check = validateDownloadUrl(m_url->text());
    if (!check.ok)
        return;

    m_probeCancel = std::make_shared<std::atomic_bool>(false);
    m_inspectedUrl = check.url.toString();
    m_inspectStatus->setText(QStringLiteral("Checking…"));
    setRole(m_inspectStatus, QStringLiteral("muted"));
    m_cancelInspect->show();

    const QUrl url = check.url;
    const std::shared_ptr<std::atomic_bool> cancel = m_probeCancel;
    const DownloadLogin credentials = login();
    m_watcher.setFuture(QtConcurrent::run([url, cancel, credentials] {
        NetworkOptions options;
        options.connectTimeoutSeconds = 15;
        options.login = credentials;
        return probeUrl(url, options, cancel.get());
    }));
}

void AddDownloadDialog::cancelInspection()
{
    m_debounce.stop();
    if (m_probeCancel)
        m_probeCancel->store(true);
    if (m_watcher.isRunning())
        m_inspectStatus->setText(QStringLiteral("Check cancelled. You can still add the download."));
    m_cancelInspect->hide();
}

void AddDownloadDialog::onInspectionFinished()
{
    m_cancelInspect->hide();
    const ProbeResult result = m_watcher.result();
    if (result.error.kind == ErrorKind::Cancelled)
        return;
    if (m_inspectedUrl != validateDownloadUrl(m_url->text()).url.toString())
        return; // the URL changed while we were checking

    if (!result.ok) {
        if (result.error.httpStatus == 401 || result.error.httpStatus == 403)
            m_loginToggle->setChecked(true);
        m_probeFailedPermanently = result.error.kind == ErrorKind::Permanent;
        m_inspectStatus->setText(result.error.message
                                 + (m_probeFailedPermanently ? QString()
                                                             : QStringLiteral(" You can still add it; Haul will retry.")));
        setRole(m_inspectStatus, m_probeFailedPermanently ? QStringLiteral("error") : QStringLiteral("warning"));
        updateButtons();
        return;
    }

    m_inspectStatus->setText(QStringLiteral("Looks good."));
    setRole(m_inspectStatus, QStringLiteral("success"));
    m_sizeValue->setText(result.totalSize >= 0 ? formatBytes(result.totalSize)
                                               : QStringLiteral("Unknown (the server did not say)"));
    m_resumeValue->setText(result.rangeSupported ? QStringLiteral("Supported")
                                                 : QStringLiteral("Not supported: pausing will restart this download"));
    m_typeValue->setText(result.contentType.isEmpty() ? QStringLiteral("—") : result.contentType);
    if (!m_nameEditedByUser && !result.suggestedName.isEmpty())
        m_fileName->setText(result.suggestedName);
    updateConflictHint();
    updateButtons();
}

void AddDownloadDialog::browseDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose download folder"), m_directory->text());
    if (!dir.isEmpty())
        m_directory->setText(QDir::toNativeSeparators(dir));
}

void AddDownloadDialog::updateConflictHint()
{
    const QString dir = m_directory->text().trimmed();
    const QString name = m_fileName->text().trimmed();
    if (dir.isEmpty() || name.isEmpty()) {
        m_conflictHint->hide();
        return;
    }
    const QString safe = sanitizeFileName(name);
    const QString unique = uniqueFileName(dir, safe);
    if (unique != safe) {
        m_conflictHint->setText(QStringLiteral("“%1” already exists in this folder. The new file will be saved as “%2” — nothing is overwritten.")
                                    .arg(safe, unique));
        m_conflictHint->show();
    } else if (safe != name) {
        m_conflictHint->setText(QStringLiteral("Unsafe characters will be replaced: the file will be saved as “%1”.").arg(safe));
        m_conflictHint->show();
    } else {
        m_conflictHint->hide();
    }
}

void AddDownloadDialog::updateButtons()
{
    const bool ok = validateDownloadUrl(m_url->text()).ok && !m_probeFailedPermanently;
    m_queueButton->setEnabled(ok);
    m_startButton->setEnabled(ok);
}

void AddDownloadDialog::submit(bool startNow)
{
    const QString checksum = m_checksum->text().trimmed();
    if (!checksum.isEmpty() && !isValidSha256Hex(checksum)) {
        m_formError->setText(QStringLiteral("The SHA-256 checksum must be exactly 64 hexadecimal characters."));
        m_formError->show();
        return;
    }
    const QString dir = QDir::fromNativeSeparators(m_directory->text().trimmed());
    if (QFileInfo::exists(dir) && !QFileInfo(dir).isWritable()) {
        m_formError->setText(QStringLiteral("You do not have permission to save files in this folder."));
        m_formError->show();
        return;
    }

    AddRequest request;
    request.url = m_url->text();
    request.directory = dir;
    request.fileName = m_fileName->text();
    request.startNow = startNow;
    request.priority = static_cast<Priority>(m_priority->currentData().toInt());
    request.expectedSha256 = checksum;
    request.login = login();

    QString error;
    m_createdId = m_manager->addDownload(request, &error);
    if (m_createdId == 0) {
        m_formError->setText(error);
        m_formError->show();
        return;
    }
    accept();
}

} // namespace haul::ui
