#include "AboutDialog.h"
#include "../MainWindow.h"
#include "ui_AboutDialog.h"
#include <QSettings>

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::AboutDialog)
{
    ui->setupUi(this);
    QSettings settings;

    ui->version_Label->setText("v" + QCoreApplication::applicationVersion());
    ui->uuid_Label->setText("<small>UUID: " + settings.value("uuid", "").toString() + "</small>");

    this->setAttribute(Qt::WA_DeleteOnClose, true);
    this->setWindowFlags(Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    this->setWindowModality(Qt::ApplicationModal);
    this->setAttribute(Qt::WA_DeleteOnClose, false);

    connect(ui->checkForUpdates_Button, &QPushButton::clicked, this, &AboutDialog::onCheckForUpdatesClicked);
}

AboutDialog::~AboutDialog()
{
    delete ui;
}

void AboutDialog::onCheckForUpdatesClicked()
{
    if (auto* mainWindow = qobject_cast<MainWindow*>(parent())) {
        mainWindow->checkForUpdates(false);
    }
}

void AboutDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
    }
}
