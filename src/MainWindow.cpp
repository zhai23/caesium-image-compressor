#include "MainWindow.h"
#include <QScopedValueRollback>
#include "utils/ImageIO.h"
#include "delegates/HtmlDelegate.h"
#include "exceptions/ImageNotSupportedException.h"
#include "exceptions/ImageTooBigException.h"
#include "filters/QSliderScrollFilter.h"
#include "ipc/CliServer.h"
#include "ui_MainWindow.h"
#include "utils/LanguageManager.h"
#include "utils/Logger.h"
#include "utils/PostCompressionActions.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QKeySequence>
#include <QLineEdit>
#include <QMovie>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>
#include <QtConcurrent>
#include <memory>
#include <dialogs/AdvancedImportDialog.h>
#include <dialogs/PreferencesDialog.h>
#include <services/Importer.h>
#include <utility>
#include <widgets/QCaesiumMessageBox.h>
#include <widgets/QCheckBoxHeaderView.h>

#ifdef Q_OS_MAC
#include "./updater/osx/CocoaInitializer.h"
#include "./updater/osx/SparkleAutoUpdater.h"
#endif

MainWindow::MainWindow(CliServer* cliServer, QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , cliServer(cliServer)
{
    qRegisterMetaType<QList<int>>();
    qRegisterMetaType<QWheelEvent*>();
    qRegisterMetaType<Qt::CheckState>();
    qRegisterMetaType<Qt::SortOrder>();

    ui->setupUi(this);
    qInfo() << "Starting UI";

    this->translator = new QTranslator();
    LanguageManager::loadLocale(translator);

    this->cImageModel = new CImageTreeModel();
    this->aboutDialog = new AboutDialog(this);
    this->compressionWatcher = new QFutureWatcher<void>();
    this->previewWatcher = new QFutureWatcher<ImagePreview>();
    this->previewRefreshTimer = new QTimer(this);
    this->previewRefreshTimer->setSingleShot(true);
    this->previewRefreshTimer->setInterval(350);
    connect(this->previewRefreshTimer, &QTimer::timeout, this, &MainWindow::doRefreshPreview);
    this->listContextMenu = new QMenu();
    this->trayIconContextMenu = new QMenu(this);
    this->networkOperations = new NetworkOperations();
    this->proxyModel = new CImageSortFilterProxyModel();
    this->trayIcon = new QSystemTrayIcon(this);

    ui->sidebar_HSplitter->setCollapsible(1, true);

    this->initListWidget();

    ui->edge_Label->hide();
    ui->edge_SpinBox->hide();

    this->keepDatesButtonGroup = new QButtonGroup();
    this->keepDatesButtonGroup->setExclusive(false);
    this->keepDatesButtonGroup->addButton(ui->keepCreationDate_CheckBox);
    this->keepDatesButtonGroup->addButton(ui->keepLastModifiedDate_CheckBox);
    this->keepDatesButtonGroup->addButton(ui->keepLastAccessDate_CheckBox);

    this->initStatusBar();
    this->initListContextMenu();
    this->initTrayIconContextMenu();
    this->initTrayIcon();
    this->setupCompressButton();
    this->setupSearchBox();

    ui->format_ComboBox->addItems(getOutputSupportedFormats());
    ui->format_ComboBox->setItemData(5,
        tr("Choose RLE or uncompressed output in the TGA compression panel. Both preserve transparency; quality sliders do not apply."), Qt::ToolTipRole);
    ui->format_ComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    this->setupChromaSubsamplingComboBox();

    ui->JPEGToggle_ToolButton->setContent(ui->JPEGOptions_Frame);
    ui->PNGToggle_ToolButton->setContent(ui->PNGOptions_Frame);
    ui->WebPToggle_ToolButton->setContent(ui->WebPOptions_Frame);
    ui->TIFFToggle_ToolButton->setContent(ui->TIFFOptions_Frame);
    ui->TGAToggle_ToolButton->setContent(ui->TGAOptions_Frame);
    connect(ui->TGAToggle_ToolButton, &QCollapseToolButton::contentVisibilityToggled, [](bool visible) {
        QSettings().setValue("mainwindow/compression/tga_options_visible", visible);
    });
    connect(ui->TGACompressionMethod_ComboBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        QSettings().setValue("compression_options/compression/tga_rle", index == 0);
        this->refreshPreview();
    });

    this->installCompressionOptionsEventFilter();

    connect(ui->JPEGToggle_ToolButton, &QCollapseToolButton::contentVisibilityToggled, this, &MainWindow::onJPEGOptionsVisibilityChanged);
    connect(ui->PNGToggle_ToolButton, &QCollapseToolButton::contentVisibilityToggled, this, &MainWindow::onPNGOptionsVisibilityChanged);
    connect(ui->WebPToggle_ToolButton, &QCollapseToolButton::contentVisibilityToggled, this, &MainWindow::onWebPOptionsVisibilityChanged);
    connect(ui->TIFFToggle_ToolButton, &QCollapseToolButton::contentVisibilityToggled, this, &MainWindow::onTIFFOptionsVisibilityChanged);

    connect(ui->imageList_TreeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::imageList_selectionChanged);
    connect(ui->imageList_TreeView, &QDropTreeView::dropFinished, this, &MainWindow::dropFinished);
    connect(this->cImageModel, &CImageTreeModel::itemsChanged, this, &MainWindow::cModelItemsChanged);
    connect(ui->preview_GraphicsView, &QZoomGraphicsView::scaleFactorChanged, ui->previewCompressed_GraphicsView, &QZoomGraphicsView::setScaleFactor);
    connect(ui->previewCompressed_GraphicsView, &QZoomGraphicsView::scaleFactorChanged, ui->preview_GraphicsView, &QZoomGraphicsView::setScaleFactor);
    connect(ui->preview_GraphicsView, &QZoomGraphicsView::previewBackgroundChangeRequested, this, &MainWindow::onPreviewBackgroundChangeRequested);
    connect(ui->previewCompressed_GraphicsView, &QZoomGraphicsView::previewBackgroundChangeRequested, this, &MainWindow::onPreviewBackgroundChangeRequested);
    connect(ui->imageList_TreeView, &QWidget::customContextMenuRequested, this, &MainWindow::showListContextMenu);
    connect(ui->imageList_TreeView->header(), &QHeaderView::sortIndicatorChanged, this, &MainWindow::listSortChanged);

    connect(ui->preview_GraphicsView->horizontalScrollBar(), &QAbstractSlider::valueChanged, ui->previewCompressed_GraphicsView, &QZoomGraphicsView::setHorizontalScrollBarValue);
    connect(ui->preview_GraphicsView->verticalScrollBar(), &QAbstractSlider::valueChanged, ui->previewCompressed_GraphicsView, &QZoomGraphicsView::setVerticalScrollBarValue);
    connect(ui->previewCompressed_GraphicsView->horizontalScrollBar(), &QAbstractSlider::valueChanged, ui->preview_GraphicsView, &QZoomGraphicsView::setHorizontalScrollBarValue);
    connect(ui->previewCompressed_GraphicsView->verticalScrollBar(), &QAbstractSlider::valueChanged, ui->preview_GraphicsView, &QZoomGraphicsView::setVerticalScrollBarValue);
    connect(keepDatesButtonGroup, &QButtonGroup::buttonClicked, this, &MainWindow::keepDatesButtonGroupClicked);

    // Refresh the preview for resize only when the user finishes editing a size
    // field (presses Enter or the field loses focus), not on every keystroke.
    connect(ui->width_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);
    connect(ui->height_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);
    connect(ui->edge_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);

    // Refresh the preview whenever any compression parameter changes.
    connect(ui->format_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPreview);
    connect(ui->lossless_CheckBox, &QCheckBox::toggled, this, &MainWindow::refreshPreview);
    connect(ui->keepMetadata_CheckBox, &QCheckBox::toggled, this, &MainWindow::refreshPreview);
    connect(ui->compressionMode_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPreview);
    connect(ui->JPEGChromaSubsampling_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPreview);
    connect(ui->JPEGProgressive_CheckBox, &QCheckBox::toggled, this, &MainWindow::refreshPreview);
    connect(ui->TIFFCompressionMethod_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPreview);
    connect(ui->maxOutputSizeUnit_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPreview);
    // Sliders: refresh only when the user releases the handle (not on every step,
    // even though the number field mirrors the slider live).
    connect(ui->JPEGQuality_Slider, &QSlider::sliderReleased, this, &MainWindow::refreshPreview);
    connect(ui->PNGQuality_Slider, &QSlider::sliderReleased, this, &MainWindow::refreshPreview);
    connect(ui->PNGOptimizationLevel_Slider, &QSlider::sliderReleased, this, &MainWindow::refreshPreview);
    connect(ui->WebPQuality_Slider, &QSlider::sliderReleased, this, &MainWindow::refreshPreview);
    connect(ui->TIFFDeflateLevel_Slider, &QSlider::sliderReleased, this, &MainWindow::refreshPreview);
    // Number fields: refresh only on Enter / focus-out (they still mirror the
    // slider live via the existing quality handlers).
    connect(ui->JPEGQuality_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);
    connect(ui->PNGQuality_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);
    connect(ui->PNGOptimizationLevel_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);
    connect(ui->WebPQuality_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);
    connect(ui->maxOutputSize_SpinBox, &QSpinBox::editingFinished, this, &MainWindow::refreshPreview);

    connect(this->previewWatcher, &QFutureWatcher<ImagePreview>::resultReadyAt, this, &MainWindow::showPreview);
    connect(this->previewWatcher, &QFutureWatcher<ImagePreview>::finished, this, &MainWindow::previewFinished);
    connect(this->previewWatcher, &QFutureWatcher<ImagePreview>::canceled, this, &MainWindow::previewCanceled);
    connect(ui->compressionMode_ComboBox, &QComboBox::currentIndexChanged, ui->compression_StackedWidget, &QStackedWidget::setCurrentIndex);
    connect(ui->compressionMode_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onCompressionModeChanged);
    connect(ui->maxOutputSize_SpinBox, &QSpinBox::valueChanged, this, &MainWindow::onMaxOutputSizeChanged);
    connect(ui->maxOutputSizeUnit_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onMaxOutputSizeUnitChanged);

    connect(ui->actionAdvanced_import, &QAction::triggered, this, &MainWindow::onAdvancedImportTriggered);

    connect(ui->moveOriginalFile_CheckBox, &QCheckBox::toggled, this, &MainWindow::moveOriginalFileToggled);
    connect(ui->moveOriginalFile_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::moveOriginalFileDestinationChanged);

    connect(ui->JPEGQuality_Slider, &QSlider::valueChanged, this, &MainWindow::onJPEGQualityValueChanged);
    connect(ui->JPEGQuality_SpinBox, &QSpinBox::valueChanged, this, &MainWindow::onJPEGQualityValueChanged);
    connect(ui->PNGQuality_Slider, &QSlider::valueChanged, this, &MainWindow::onPNGQualityValueChanged);
    connect(ui->PNGQuality_SpinBox, &QSpinBox::valueChanged, this, &MainWindow::onPNGQualityValueChanged);
    connect(ui->WebPQuality_Slider, &QSlider::valueChanged, this, &MainWindow::onWebPQualityValueChanged);
    connect(ui->WebPQuality_SpinBox, &QSpinBox::valueChanged, this, &MainWindow::onWebPQualityValueChanged);
    connect(ui->JPEGChromaSubsampling_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onJPEGChromaSubsamplingChanged);
    connect(ui->JPEGProgressive_CheckBox, &QCheckBox::toggled, this, &MainWindow::onJPEGProgressiveToggled);
    connect(ui->PNGOptimizationLevel_Slider, &QSlider::valueChanged, this, &MainWindow::onPNGOptimizationLevelChanged);
    connect(ui->PNGOptimizationLevel_SpinBox, &QSpinBox::valueChanged, this, &MainWindow::onPNGOptimizationLevelChanged);

    connect(ui->TIFFCompressionMethod_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onTIFFCompressionMethodChanged);
    connect(ui->TIFFDeflateLevel_Slider, &QSlider::valueChanged, this, &MainWindow::onTIFFDeflateLevelChanged);
    this->readSettings();

    connect(ui->format_ComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::outputFormatIndexChanged);

    this->on_keepAspectRatio_CheckBox_toggled(ui->keepAspectRatio_CheckBox->isChecked());
    this->on_doNotEnlarge_CheckBox_toggled(ui->doNotEnlarge_CheckBox->isChecked());
    this->on_keepAspectRatio_CheckBox_toggled(ui->keepAspectRatio_CheckBox->isChecked());
    MainWindow::on_sameOutputFolderAsInput_CheckBox_toggled(ui->sameOutputFolderAsInput_CheckBox->isChecked());
    this->moveOriginalFileToggled(ui->moveOriginalFile_CheckBox->isChecked());
    this->on_fitTo_ComboBox_currentIndexChanged(ui->fitTo_ComboBox->currentIndex());
    this->toggleLosslessWarningVisible();

    ui->actionToolbarIcons_only->setChecked(ui->toolBar->toolButtonStyle() == Qt::ToolButtonIconOnly && !ui->toolBar->isHidden());
    ui->actionToolbarIcons_and_Text->setChecked(ui->toolBar->toolButtonStyle() == Qt::ToolButtonTextUnderIcon && !ui->toolBar->isHidden());
    ui->actionToolbarHide->setChecked(ui->toolBar->isHidden());

    if (QSettings().value("preferences/general/send_usage_reports", true).toBool()) {
        if (!QSettings().contains("access_token")) {
            this->networkOperations->requestToken();
        } else {
            this->networkOperations->updateSystemInfo();
        }
    }

    QCommandLineParser commandLineParser;
    commandLineParser.process(QApplication::arguments());
    if (const QStringList args = commandLineParser.positionalArguments(); !args.isEmpty()) {
        this->importFromArgs(args);
    }

    if (this->cliServer != nullptr) {
        this->cliServer->setParent(this);
        this->cliServer->setHandler([this](const QString& method, const QJsonObject& params) {
            return this->handleCliRequest(method, params);
        });
    }

    QImageReader::setAllocationLimit(1024);
}

MainWindow::~MainWindow()
{
    delete proxyModel;
    delete cImageModel;
    delete aboutDialog;
    delete keepDatesButtonGroup;
    delete compressionWatcher;
    delete networkOperations;
    delete previewWatcher;
    delete trayIcon;
    delete translator;
    delete ui;
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);

    this->updateCheckBoxColumnWidth();
    this->initUpdater();
}

void MainWindow::initStatusBar() const
{
    ui->statusbar->addPermanentWidget(ui->losslessWarning_Button);
    ui->statusbar->addPermanentWidget(ui->compressionProgress_Label);
    ui->statusbar->addPermanentWidget(ui->compression_ProgressBar);
    ui->statusbar->addPermanentWidget(ui->cancelCompression_Button);

    ui->losslessWarning_Button->hide();
    ui->cancelCompression_Button->hide();
    ui->compression_ProgressBar->hide();
    ui->compressionProgress_Label->hide();
}

void MainWindow::initListContextMenu()
{
    this->listContextMenu->addAction(ui->actionAdd_files);
    this->listContextMenu->addAction(ui->actionAdd_folder);
    this->listContextMenu->addSeparator();
    this->listContextMenu->addAction(ui->actionRemove);
    this->listContextMenu->addAction(ui->actionClear);
    this->listContextMenu->addSeparator();
    this->listContextMenu->addAction(ui->actionShow_original_in_file_manager);
    this->listContextMenu->addAction(ui->actionShow_compressed_in_file_manager);
    this->listContextMenu->addSeparator();
    this->listContextMenu->addAction(ui->actionRestore);

    connect(this->listContextMenu, &QMenu::aboutToShow, this, &MainWindow::listContextMenuAboutToShow);
}

void MainWindow::initTrayIconContextMenu()
{
    this->trayIconContextMenu->addAction(ui->actionShow);
    this->trayIconContextMenu->addSeparator();
    this->trayIconContextMenu->addAction(ui->actionExit);
    connect(ui->actionShow, &QAction::triggered, this, &MainWindow::restoreWindow);
}

void MainWindow::initListWidget() const
{
    int defaultSectionSize = ui->imageList_TreeView->header()->defaultSectionSize();
    this->proxyModel->setSourceModel(this->cImageModel);
    ui->imageList_TreeView->setModel(this->proxyModel);
    ui->imageList_TreeView->setIconSize(QSize(10, 10));
    // Every row is the same height (the HTML delegate returns a fixed height),
    // so enable the uniform-row-height optimization. Without it, locating a row
    // (visualRect) is O(row), making batch updates like select/deselect-all
    // O(n^2) and feel laggy.
    ui->imageList_TreeView->setUniformRowHeights(true);

    auto* checkBoxHeader = new QCheckBoxHeaderView(CImageColumns::CHECKBOX_COLUMN, Qt::Horizontal, ui->imageList_TreeView);
    checkBoxHeader->setStretchLastSection(false);
    checkBoxHeader->setSortIndicatorShown(true);
    ui->imageList_TreeView->setHeader(checkBoxHeader);
    connect(checkBoxHeader, &QCheckBoxHeaderView::checkStateToggled, this, &MainWindow::onHeaderCheckStateToggled);
    connect(this->cImageModel, &CImageTreeModel::checkedItemsChanged, this, &MainWindow::updateHeaderCheckState);

    // Make the checkbox column square: width equals the header height so the
    // header cell (and the checkbox inside it) is a square.
    // The header enforces a global minimumSectionSize (~30px by default); lower
    // it so the column can shrink. The exact width is applied again in
    // showEvent() once the header has its final height.
    ui->imageList_TreeView->header()->setMinimumSectionSize(16);
    ui->imageList_TreeView->header()->setSectionResizeMode(CImageColumns::CHECKBOX_COLUMN, QHeaderView::Fixed);
    this->updateCheckBoxColumnWidth();
    ui->imageList_TreeView->header()->resizeSection(CImageColumns::NAME_COLUMN, QSettings().value("mainwindow/list_view/header_column_size/name", 250).toInt());
    ui->imageList_TreeView->header()->resizeSection(CImageColumns::SIZE_COLUMN, QSettings().value("mainwindow/list_view/header_column_size/size", defaultSectionSize).toInt());
    ui->imageList_TreeView->header()->resizeSection(CImageColumns::RESOLUTION_COLUMN, QSettings().value("mainwindow/list_view/header_column_size/resolution", defaultSectionSize).toInt());
    ui->imageList_TreeView->header()->resizeSection(CImageColumns::RATIO_COLUMN, QSettings().value("mainwindow/list_view/header_column_size/ratio", defaultSectionSize).toInt());

    ui->imageList_TreeView->header()->setSortIndicator(QSettings().value("mainwindow/list_view/sort_column_index", 0).toInt(), QSettings().value("mainwindow/list_view/sort_column_order", Qt::AscendingOrder).value<Qt::SortOrder>());

    // Apply the HTML delegate to the data columns only; the checkbox column keeps
    // the default delegate so the check indicator is rendered natively.
    auto* htmlDelegate = new HtmlDelegate();
    ui->imageList_TreeView->setItemDelegateForColumn(CImageColumns::NAME_COLUMN, htmlDelegate);
    ui->imageList_TreeView->setItemDelegateForColumn(CImageColumns::SIZE_COLUMN, htmlDelegate);
    ui->imageList_TreeView->setItemDelegateForColumn(CImageColumns::RESOLUTION_COLUMN, htmlDelegate);
    ui->imageList_TreeView->setItemDelegateForColumn(CImageColumns::RATIO_COLUMN, htmlDelegate);
    ui->imageList_TreeView->setItemDelegateForColumn(CImageColumns::INFO_COLUMN, htmlDelegate);

    this->applyMultiSelectionVisibility();
    this->applyPreviewBackground();
}

void MainWindow::updateCheckBoxColumnWidth() const
{
    // Square checkbox column: width == header height.
    auto* header = ui->imageList_TreeView->header();
    int size = header->height();
    if (size <= 0) {
        size = header->sizeHint().height();
    }
    if (size <= 0) {
        size = 24;
    }
    header->resizeSection(CImageColumns::CHECKBOX_COLUMN, size);
}

void MainWindow::applyMultiSelectionVisibility() const
{
    bool enabled = QSettings().value("preferences/general/enable_multi_selection", false).toBool();
    ui->imageList_TreeView->setColumnHidden(CImageColumns::CHECKBOX_COLUMN, !enabled);
    if (enabled) {
        this->updateCheckBoxColumnWidth();
    }
}

void MainWindow::setMultiSelectionEnabled(bool enabled)
{
    QSettings().setValue("preferences/general/enable_multi_selection", enabled);
    if (!enabled) {
        // Disabling multi-selection means "process everything": force-select
        // all images so none stays deselected and unselectable (the checkbox
        // column is hidden).
        this->cImageModel->setAllChecked(true);
    }
    this->applyMultiSelectionVisibility();
}

void MainWindow::applyPreviewBackground() const
{
    int mode = QSettings().value("preferences/general/preview_background", 0).toInt();
    ui->preview_GraphicsView->setPreviewBackground(mode);
    ui->previewCompressed_GraphicsView->setPreviewBackground(mode);
}

void MainWindow::onPreviewBackgroundChangeRequested(int mode) const
{
    QSettings().setValue("preferences/general/preview_background", mode);
    this->applyPreviewBackground();
}

void MainWindow::initTrayIcon()
{
#ifdef Q_OS_MAC
    QIcon icon = QIcon(":/icons/logo_mono.png");
    icon.setIsMask(true);
#else
    QIcon icon = QIcon(":/icons/logo.png");
#endif

    this->trayIcon->setIcon(icon);
    this->trayIcon->setContextMenu(this->trayIconContextMenu);
    this->trayIcon->setToolTip(QCoreApplication::applicationName());
    connect(this->trayIcon, &QSystemTrayIcon::messageClicked, this, &MainWindow::restoreWindow);
    connect(this->trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            this->restoreWindow();
        }
    });
    this->trayIcon->show();
}

void MainWindow::restoreWindow()
{
    this->setWindowState(this->windowState() & ~Qt::WindowMinimized);
    this->show();
    this->raise();
    this->activateWindow();
}

bool MainWindow::minimizeToTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return false;
    this->trayIcon->show();
    this->writeSettings();
    this->hide();
    return true;
}

void MainWindow::on_actionExit_triggered()
{
    const QScopedValueRollback<bool> requested(this->explicitExitRequested, true);
    if (this->isHidden()) this->restoreWindow();
    this->close();
}

void MainWindow::on_actionAbout_Caesium_Image_Compressor_triggered() const
{
    aboutDialog->setWindowModality(Qt::NonModal);
    aboutDialog->show();
}

void MainWindow::on_addFiles_Button_clicked()
{
    return MainWindow::triggerImportFiles();
}

void MainWindow::on_actionAdd_files_triggered()
{
    return MainWindow::triggerImportFiles();
}

void MainWindow::triggerImportFiles()
{
    // Generate file dialog for import and call the progress dialog indicator
    QStringList fileList = QFileDialog::getOpenFileNames(this,
        tr("Import files..."),
        this->lastOpenedDirectory,
        QIODevice::tr("Image Files") + " (*.jpg *.jpeg *.png *.webp *.tif *.tiff *.tga)");

    if (fileList.isEmpty()) {
        return;
    }

    QString baseFolder = QFileInfo(fileList.at(0)).absolutePath();
    this->lastOpenedDirectory = baseFolder;

    return this->importFiles(fileList, baseFolder);
}

void MainWindow::triggerImportFolder()
{
    QString directoryPath = QFileDialog::getExistingDirectory(this, tr("Import folder..."),
        this->lastOpenedDirectory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (directoryPath.isEmpty()) {
        return;
    }

    bool scanSubfolders = QSettings().value("preferences/general/import_subfolders", true).toBool();
    QStringList fileList = Importer::scanDirectory(directoryPath, scanSubfolders);

    if (fileList.isEmpty()) {
        return;
    }

    this->lastOpenedDirectory = directoryPath;
    return this->importFiles(fileList, directoryPath);
}

void MainWindow::writeSettings() const
{
    QSettings().setValue("mainwindow/geometry", this->saveGeometry());
    QSettings().setValue("mainwindow/window_state", this->saveState());
    QSettings().setValue("mainwindow/left_splitter_sizes", QVariant::fromValue<QList<int>>(ui->sidebar_HSplitter->sizes()));
    QSettings().setValue("mainwindow/previews_visible", ui->actionShow_previews->isChecked());
    QSettings().setValue("mainwindow/auto_preview", ui->actionAuto_preview->isChecked());

    if (ui->actionShow_previews->isChecked()) {
        QSettings().setValue("mainwindow/main_splitter_sizes", QVariant::fromValue<QList<int>>(ui->main_VSplitter->sizes()));
    }
    QSettings().setValue("mainwindow/list_view/header_column_size/name", ui->imageList_TreeView->header()->sectionSize(CImageColumns::NAME_COLUMN));
    QSettings().setValue("mainwindow/list_view/header_column_size/size", ui->imageList_TreeView->header()->sectionSize(CImageColumns::SIZE_COLUMN));
    QSettings().setValue("mainwindow/list_view/header_column_size/resolution", ui->imageList_TreeView->header()->sectionSize(CImageColumns::RESOLUTION_COLUMN));
    QSettings().setValue("mainwindow/list_view/header_column_size/ratio", ui->imageList_TreeView->header()->sectionSize(CImageColumns::RATIO_COLUMN));
    QSettings().setValue("mainwindow/list_view/sort_column_index", ui->imageList_TreeView->header()->sortIndicatorSection());
    QSettings().setValue("mainwindow/list_view/sort_column_order", ui->imageList_TreeView->header()->sortIndicatorOrder());
    QSettings().setValue("mainwindow/search/regex_mode", this->searchRegexToolButton->isChecked());
    QSettings().setValue("mainwindow/search/case_sensitive", this->searchCaseToolButton->isChecked());
    QSettings().setValue("mainwindow/toolbar/visible", ui->toolBar->isVisible());
    QSettings().setValue("mainwindow/toolbar/button_style", ui->toolBar->toolButtonStyle());
    QSettings().setValue("mainwindow/compression/jpeg_options_visible", ui->JPEGToggle_ToolButton->contentVisible());
    QSettings().setValue("mainwindow/compression/png_options_visible", ui->PNGToggle_ToolButton->contentVisible());
    QSettings().setValue("mainwindow/compression/webp_options_visible", ui->WebPToggle_ToolButton->contentVisible());
    QSettings().setValue("mainwindow/compression/tiff_options_visible", ui->TIFFToggle_ToolButton->contentVisible());
    QSettings().setValue("mainwindow/compression/tga_options_visible", ui->TGAToggle_ToolButton->contentVisible());
    QSettings().setValue("compression_options/compression/tga_rle", ui->TGACompressionMethod_ComboBox->currentIndex() == 0);

    QSettings().setValue("compression_options/compression/mode", ui->compressionMode_ComboBox->currentIndex());
    QSettings().setValue("compression_options/compression/lossless", ui->lossless_CheckBox->isChecked());
    QSettings().setValue("compression_options/compression/keep_metadata", ui->keepMetadata_CheckBox->isChecked());
    QSettings().setValue("compression_options/compression/keep_structure", ui->keepStructure_CheckBox->isChecked());
    QSettings().setValue("compression_options/compression/jpeg_quality", ui->JPEGQuality_Slider->value());
    QSettings().setValue("compression_options/compression/jpeg_chroma_subsampling", ui->JPEGChromaSubsampling_ComboBox->currentData(Qt::UserRole));
    QSettings().setValue("compression_options/compression/jpeg_progressive", ui->JPEGProgressive_CheckBox->isChecked());
    QSettings().setValue("compression_options/compression/png_quality", ui->PNGQuality_Slider->value());
    QSettings().setValue("compression_options/compression/png_optimization_level", ui->PNGOptimizationLevel_Slider->value());
    QSettings().setValue("compression_options/compression/webp_quality", ui->WebPQuality_Slider->value());
    QSettings().setValue("compression_options/compression/tiff_method", ui->TIFFCompressionMethod_ComboBox->currentIndex());
    QSettings().setValue("compression_options/compression/tiff_deflate_level", ui->TIFFDeflateLevel_Slider->value());
    QSettings().setValue("compression_options/compression/max_output_size", ui->maxOutputSize_SpinBox->value());
    QSettings().setValue("compression_options/compression/max_output_size_unit", ui->maxOutputSizeUnit_ComboBox->currentIndex());

    QSettings().setValue("compression_options/resize/resize", ui->fitTo_ComboBox->currentIndex() != ResizeMode::NO_RESIZE);
    QSettings().setValue("compression_options/resize/fit_to", ui->fitTo_ComboBox->currentIndex());
    QSettings().setValue("compression_options/resize/width", ui->width_SpinBox->value());
    QSettings().setValue("compression_options/resize/height", ui->height_SpinBox->value());
    QSettings().setValue("compression_options/resize/size", ui->edge_SpinBox->value());
    QSettings().setValue("compression_options/resize/keep_aspect_ratio", ui->keepAspectRatio_CheckBox->isChecked());
    QSettings().setValue("compression_options/resize/do_not_enlarge", ui->doNotEnlarge_CheckBox->isChecked());

    QSettings().setValue("compression_options/output/output_folder", ui->outputFolder_LineEdit->text());
    QSettings().setValue("compression_options/output/output_suffix", ui->outputSuffix_LineEdit->text());
    QSettings().setValue("compression_options/output/same_folder_as_input", ui->sameOutputFolderAsInput_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/skip_if_bigger", ui->skipIfBigger_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/cache_originals", ui->cacheOriginals_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/delete_original_format_file", ui->deleteOriginalFormatFile_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/sync_meta_file", ui->syncMetaFile_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/sync_spine_file", ui->syncSpineFile_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/sync_font_atlas_files", ui->syncFontAtlasFiles_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/keep_dates", ui->keepDates_CheckBox->checkState());
    QSettings().setValue("compression_options/output/keep_creation_date", ui->keepCreationDate_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/keep_last_modified_date", ui->keepLastModifiedDate_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/keep_last_access_date", ui->keepLastAccessDate_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/format", ui->format_ComboBox->currentIndex());
    QSettings().setValue("compression_options/output/move_original_file", ui->moveOriginalFile_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/move_original_file_destination", ui->moveOriginalFile_ComboBox->currentIndex());

    QSettings().setValue("extra/last_opened_directory", this->lastOpenedDirectory);
}

void MainWindow::readSettings()
{
    this->restoreGeometry(QSettings().value("mainwindow/geometry").toByteArray());
    this->restoreState(QSettings().value("mainwindow/window_state").toByteArray());
    ui->sidebar_HSplitter->setSizes(QSettings().value("mainwindow/left_splitter_sizes", QVariant::fromValue<QList<int>>({ 600, 1 })).value<QList<int>>());
    ui->main_VSplitter->setSizes(QSettings().value("mainwindow/main_splitter_sizes", QVariant::fromValue<QList<int>>({ 500, 250 })).value<QList<int>>());
    ui->actionShow_previews->setChecked(QSettings().value("mainwindow/previews_visible", true).toBool());
    ui->actionAuto_preview->setChecked(QSettings().value("mainwindow/auto_preview", false).toBool());
    ui->toolBar->setVisible(QSettings().value("mainwindow/toolbar/visible", true).toBool());
    ui->toolBar->setToolButtonStyle(QSettings().value("mainwindow/toolbar/button_style", Qt::ToolButtonIconOnly).value<Qt::ToolButtonStyle>());
    this->searchRegexToolButton->setChecked(QSettings().value("mainwindow/search/regex_mode", false).toBool());
    this->searchCaseToolButton->setChecked(QSettings().value("mainwindow/search/case_sensitive", false).toBool());
    ui->JPEGToggle_ToolButton->setContentVisible(QSettings().value("mainwindow/compression/jpeg_options_visible", true).toBool());
    ui->PNGToggle_ToolButton->setContentVisible(QSettings().value("mainwindow/compression/png_options_visible", true).toBool());
    ui->WebPToggle_ToolButton->setContentVisible(QSettings().value("mainwindow/compression/webp_options_visible", true).toBool());
    ui->TIFFToggle_ToolButton->setContentVisible(QSettings().value("mainwindow/compression/tiff_options_visible", true).toBool());
    ui->TGAToggle_ToolButton->setContentVisible(QSettings().value("mainwindow/compression/tga_options_visible", true).toBool());
    ui->TGACompressionMethod_ComboBox->setCurrentIndex(QSettings().value("compression_options/compression/tga_rle", true).toBool() ? 0 : 1);

    ui->compressionMode_ComboBox->setCurrentIndex(QSettings().value("compression_options/compression/mode", 0).toInt());
    ui->lossless_CheckBox->setChecked(QSettings().value("compression_options/compression/lossless", false).toBool());
    ui->keepMetadata_CheckBox->setChecked(QSettings().value("compression_options/compression/keep_metadata", true).toBool());
    ui->keepStructure_CheckBox->setChecked(QSettings().value("compression_options/compression/keep_structure", false).toBool());
    ui->JPEGQuality_Slider->setValue(QSettings().value("compression_options/compression/jpeg_quality", 80).toInt());
    ui->JPEGChromaSubsampling_ComboBox->setCurrentIndex(static_cast<int>(getChromaSubsamplingOptions().keys().indexOf(QSettings().value("compression_options/compression/jpeg_chroma_subsampling", 0).toInt())));
    ui->JPEGProgressive_CheckBox->setChecked(QSettings().value("compression_options/compression/jpeg_progressive", true).toBool());
    ui->PNGQuality_SpinBox->setValue(QSettings().value("compression_options/compression/png_quality", 80).toInt());
    ui->PNGOptimizationLevel_Slider->setValue(QSettings().value("compression_options/compression/png_optimization_level", 3).toInt());
    ui->JPEGQuality_SpinBox->setValue(QSettings().value("compression_options/compression/jpeg_quality", 80).toInt());
    ui->WebPQuality_SpinBox->setValue(QSettings().value("compression_options/compression/webp_quality", 60).toInt());
    ui->TIFFCompressionMethod_ComboBox->setCurrentIndex(QSettings().value("compression_options/compression/tiff_method", 1).toInt());
    ui->TIFFDeflateLevel_Slider->setValue(QSettings().value("compression_options/compression/tiff_deflate_level", 2).toInt());
    this->updateTIFFDeflateLevelLabel(ui->TIFFDeflateLevel_Slider->value());
    ui->maxOutputSize_SpinBox->setValue(QSettings().value("compression_options/compression/max_output_size", 500).toInt());
    ui->maxOutputSizeUnit_ComboBox->setCurrentIndex(QSettings().value("compression_options/compression/max_output_size_unit", 0).toInt());

    ui->fitTo_ComboBox->setCurrentIndex(QSettings().value("compression_options/resize/fit_to", 0).toInt());
    ui->width_SpinBox->setValue(QSettings().value("compression_options/resize/width", 1000).toInt());
    ui->height_SpinBox->setValue(QSettings().value("compression_options/resize/height", 1000).toInt());
    ui->edge_SpinBox->setValue(QSettings().value("compression_options/resize/size", 1000).toInt());
    ui->keepAspectRatio_CheckBox->setChecked(QSettings().value("compression_options/resize/keep_aspect_ratio", false).toBool());
    ui->doNotEnlarge_CheckBox->setChecked(QSettings().value("compression_options/resize/do_not_enlarge", false).toBool());

    ui->outputFolder_LineEdit->setText(QSettings().value("compression_options/output/output_folder", "").toString());
    ui->outputSuffix_LineEdit->setText(QSettings().value("compression_options/output/output_suffix", "").toString());
    ui->sameOutputFolderAsInput_CheckBox->setChecked(QSettings().value("compression_options/output/same_folder_as_input", false).toBool());
    ui->skipIfBigger_CheckBox->setChecked(QSettings().value("compression_options/output/skip_if_bigger", true).toBool());
    ui->cacheOriginals_CheckBox->setChecked(QSettings().value("compression_options/output/cache_originals", false).toBool());
    ui->deleteOriginalFormatFile_CheckBox->setChecked(QSettings().value("compression_options/output/delete_original_format_file", true).toBool());
    ui->syncMetaFile_CheckBox->setChecked(QSettings().value("compression_options/output/sync_meta_file", false).toBool());
    ui->syncSpineFile_CheckBox->setChecked(QSettings().value("compression_options/output/sync_spine_file", false).toBool());
    ui->syncFontAtlasFiles_CheckBox->setChecked(QSettings().value("compression_options/output/sync_font_atlas_files", false).toBool());
    ui->keepDates_CheckBox->setCheckState(QSettings().value("compression_options/output/keep_dates", Qt::Unchecked).value<Qt::CheckState>());
    ui->keepCreationDate_CheckBox->setChecked(QSettings().value("compression_options/output/keep_creation_date", false).toBool());
    ui->keepLastModifiedDate_CheckBox->setChecked(QSettings().value("compression_options/output/keep_last_modified_date", false).toBool());
    ui->keepLastAccessDate_CheckBox->setChecked(QSettings().value("compression_options/output/keep_last_access_date", false).toBool());
    ui->format_ComboBox->setCurrentIndex(QSettings().value("compression_options/output/format", 0).toInt());
    ui->moveOriginalFile_CheckBox->setChecked(QSettings().value("compression_options/output/move_original_file", QSettings().value("compression_options/output/move_original_to_trash", false).toBool()).toBool()); // Trying to get legacy value
    ui->moveOriginalFile_ComboBox->setCurrentIndex(QSettings().value("compression_options/output/move_original_file_destination", 0).toInt());

    this->lastOpenedDirectory = QSettings().value("extra/last_opened_directory", QStandardPaths::standardLocations(QStandardPaths::PicturesLocation).at(0)).toString();
}

void MainWindow::previewImage(const QModelIndex& imageIndex, bool forceRuntimePreview) const
{
    if (this->previewWatcher->isRunning()) {
        this->previewWatcher->cancel();
        this->previewWatcher->waitForFinished();
    }
    if (!QSettings().value("mainwindow/previews_visible", false).toBool()) {
        return;
    }
    ui->preview_GraphicsView->removePixmap();
    ui->previewCompressed_GraphicsView->removePixmap();

    ui->preview_GraphicsView->resetScaleFactor();
    ui->preview_GraphicsView->setLoading(true);
    ui->originalImageSize_Label->setLoading(true);
    ui->previewCompressed_GraphicsView->resetScaleFactor();

    ui->actionPreview->setEnabled(false);

    CImage* cImage = this->cImageModel->getRootItem()->children().at(imageIndex.row())->getCImage();
    QString imageToBePreviewed = forceRuntimePreview ? cImage->getTemporaryPreviewFullPath() : cImage->getCompressedFullPath();
    // When "cache originals" is on, the original-preview pane should show the
    // cached pristine copy (the on-disk original may already be overwritten).
    bool cacheOriginals = QSettings().value("compression_options/output/cache_originals", false).toBool();
    QString originalPreviewPath = cImage->getCompressionSourcePath(cacheOriginals);
    QList<std::pair<QString, bool>> images;
    images.append(std::pair<QString, bool>(originalPreviewPath, false));

    // Determine the output format shown for the compressed pane (conversion
    // target, or the original format when "Same as input").
    int outputFormatIndex = ui->format_ComboBox->currentIndex();
    QString compressedFormat = outputFormatIndex == 0
        ? cImage->getFormat().toUpper()
        : getOutputSupportedFormats().at(outputFormatIndex).toUpper();
    QString originalFormat = cImage->getFormat().toUpper();

    // TODO Manage failure better
    std::function<ImagePreview(std::pair<QString, bool>)> loadPixmap = [this, forceRuntimePreview, cImage, compressedFormat, originalFormat](const std::pair<QString, bool>& pair) {
        QString previewFullPath = pair.first;
        ImagePreview imagePreview;
        bool isOnFlyPreview = false;
        if (pair.second && forceRuntimePreview && !QFileInfo::exists(previewFullPath)) {
            isOnFlyPreview = true;
            bool result = cImage->preview(this->getCompressionOptions(this->importedFilesRootFolder));
            if (!result) {
                previewFullPath = cImage->getCompressedFullPath();
            }
        }
        QString detectedFormat = ImageIO::format(previewFullPath).toUpper();
        QPixmap image = QPixmap::fromImage(ImageIO::read(previewFullPath));
        imagePreview.image = image;
        imagePreview.fileInfo = QFileInfo(previewFullPath);
        imagePreview.originalSize = cImage->getOriginalSize();
        imagePreview.isOnFlyPreview = isOnFlyPreview;
        // pair.second == true -> compressed pane; false -> original pane.
        QString format = pair.second ? compressedFormat : originalFormat;
        if (format.isEmpty()) {
            format = detectedFormat;
        }
        imagePreview.format = format;
        return imagePreview;
    };

    if (!cImage->getCompressedFullPath().isEmpty() || forceRuntimePreview) {
        images.append(std::pair<QString, bool>(imageToBePreviewed, true));
        ui->previewCompressed_GraphicsView->setLoading(true);
        ui->compressedImageSize_Label->setLoading(true);
    } else {
        ui->previewCompressed_GraphicsView->setLoading(false);
        ui->compressedImageSize_Label->setLoading(false);
        ui->previewCompressed_GraphicsView->scene()->setSceneRect(ui->previewCompressed_GraphicsView->scene()->itemsBoundingRect());
        ui->previewCompressed_GraphicsView->fitInView(ui->previewCompressed_GraphicsView->scene()->itemsBoundingRect(), Qt::KeepAspectRatio);
        ui->previewCompressed_GraphicsView->show();
    }
    ui->preview_GraphicsView->setZoomEnabled(false);
    ui->previewCompressed_GraphicsView->setZoomEnabled(false);

    this->previewWatcher->setFuture(QtConcurrent::mapped(images, loadPixmap));
}

void MainWindow::updateFolderMap(QString baseFolder, int count)
{
    QStringListIterator it(this->folderMap.keys());
    bool isAlreadyInList = false;
    while (it.hasNext() && !isAlreadyInList) {
        QString folderInMap = it.next();
        isAlreadyInList = baseFolder.startsWith(folderInMap);
        if (isAlreadyInList) {
            baseFolder = folderInMap;
        }
    }

    if (!isAlreadyInList) {
        this->folderMap.insert(baseFolder, count);
    } else {
        this->folderMap[baseFolder] += count;
    }

    if (this->folderMap[baseFolder] == 0) {
        this->folderMap.remove(baseFolder);
    }
}

void MainWindow::importFiles(const QStringList& fileList, QString baseFolder, bool showProgress)
{
    int listLength = static_cast<int>(fileList.count());
    std::unique_ptr<QProgressDialog> progressDialog;
    if (showProgress) {
        progressDialog = std::make_unique<QProgressDialog>(tr("Importing files..."), tr("Cancel"), 0, listLength, this);
        progressDialog->setWindowModality(Qt::WindowModal);
    }

    QList<CImage*> list;
    for (int i = 0; i < listLength; i++) {
        if (showProgress && progressDialog->wasCanceled()) {
            break;
        }

        try {
            auto* cImage = new CImage(fileList.at(i));
            if (this->cImageModel->contains(cImage)) {
                continue;
            }
            cImage->setCliId(this->nextCliId++);
            list.append(cImage);
        } catch (ImageNotSupportedException& e) {
            qWarning() << fileList.at(i) << "is not supported. Error:" << e.what();
        } catch (ImageTooBigException& e) {
            qWarning() << fileList.at(i) << "is too big. Error:" << e.what();
        }

        if (showProgress) {
            progressDialog->setValue(i);
        }
    }

    if (!list.isEmpty() && listLength > 0) {
        this->updateFolderMap(std::move(baseFolder), static_cast<int>(list.count()));
        QString rootFolder = Importer::getRootFolder(this->folderMap.keys());
        this->cImageModel->appendItems(list, rootFolder);
        this->importedFilesRootFolder = rootFolder;
    }

    if (showProgress) {
        progressDialog->setValue(listLength);
    }
}

void MainWindow::removeFiles(bool all)
{
    this->isItemRemovalRunning = true;
    if (all) {
        ui->imageList_TreeView->selectAll();
    }
    QModelIndexList indexes = ui->imageList_TreeView->selectionModel()->selectedIndexes();
    std::sort(indexes.begin(), indexes.end(), [](const QModelIndex& a, const QModelIndex& b) {
        return a.row() < b.row();
    });

    int columnCount = this->cImageModel->columnCount();

    for (long long i = indexes.count() / columnCount; i > 0; i--) {
        auto currentIndex = this->proxyModel->mapToSource(indexes.at(i));
        auto indexRow = currentIndex.row();
        auto indexParent = currentIndex.parent();
        auto* cImage = this->cImageModel->getRootItem()->children().at(indexRow)->getCImage();
        this->updateFolderMap(cImage->getFullPath(), -1);
        // Drop the cached original copy (if any) for the removed image.
        cImage->removeCachedOriginal();
        this->cImageModel->removeRows(indexRow, 1, indexParent);
    }
    this->previewWatcher->cancel();
    ui->preview_GraphicsView->removePixmap();
    ui->previewCompressed_GraphicsView->removePixmap();
    ui->preview_GraphicsView->scene()->setSceneRect(ui->preview_GraphicsView->scene()->itemsBoundingRect());
    ui->previewCompressed_GraphicsView->scene()->setSceneRect(ui->previewCompressed_GraphicsView->scene()->itemsBoundingRect());
    this->isItemRemovalRunning = false;
}

void MainWindow::on_compress_Button_clicked()
{
    this->startCompression();
}

void MainWindow::startCompression(bool onlyFailed, const QList<CImage*>& targets)
{

    if (ui->outputFolder_LineEdit->text().isEmpty() && !ui->sameOutputFolderAsInput_CheckBox->isChecked()) {
        if (this->cliMode) {
            this->cliErrorCode = "NO_OUTPUT_FOLDER";
            this->cliErrorMessage = tr("Please select an output folder first");
            return;
        }
        QCaesiumMessageBox msgBox;
        msgBox.setText(tr("Please select an output folder first"));
        msgBox.addButton(tr("Ok"), QMessageBox::AcceptRole);
        msgBox.exec();
        return;
    }

    QString rootFolder = Importer::getRootFolder(this->folderMap.keys());

    bool skipDialogs = QSettings().value("preferences/general/skip_compression_dialogs").toBool();
    bool overwriteWarningFlag = (ui->sameOutputFolderAsInput_CheckBox->isChecked() && ui->outputSuffix_LineEdit->text().isEmpty())
        || rootFolder == ui->outputFolder_LineEdit->text();

    if (overwriteWarningFlag && !skipDialogs && !this->cliMode) {
        QCaesiumMessageBox sameFolderPrompt;
        sameFolderPrompt.setText(tr("You are about to overwrite your original images and this action can't be undone.\n\nDo you really want to continue?"));
        sameFolderPrompt.addButton(tr("Yes"), QMessageBox::YesRole);
        auto noButton = sameFolderPrompt.addButton(tr("No"), QMessageBox::NoRole);

        sameFolderPrompt.exec();

        if (sameFolderPrompt.clickedButton() == noButton) {
            return;
        }
    }
    int totalImages = this->cImageModel->getRootItem()->childCount();

    if (totalImages == 0) {
        if (this->cliMode) {
            this->cliErrorCode = "NO_IMAGES";
            this->cliErrorMessage = tr("No images in the list");
        }
        return;
    }

    bool multiSelectionEnabled = QSettings().value("preferences/general/enable_multi_selection", false).toBool();

    // A search filter narrows the list to what is visible: pressing Compress
    // acts on the rows the user is looking at, never on the hidden ones. An
    // explicit target list (CLI by ID/path) is never narrowed.
    QList<CImage*> effectiveTargets = targets;
    const bool narrowedBySearch = !this->cliMode && targets.isEmpty() && this->proxyModel->hasSearchFilter();
    if (narrowedBySearch) {
        effectiveTargets = this->visibleImages();
        if (effectiveTargets.isEmpty()) {
            QCaesiumMessageBox msgBox(this);
            if (this->proxyModel->rowCount() == 0) {
                msgBox.setText(tr("No images match the current search"));
                msgBox.setInformativeText(tr("Please adjust the search to include the images you want to compress."));
            } else {
                msgBox.setText(tr("No images selected"));
                msgBox.setInformativeText(tr("Please check at least one image to compress."));
            }
            msgBox.addButton(tr("Ok"), QMessageBox::AcceptRole);
            msgBox.exec();
            return;
        }
    }

    // When an explicit target list is given (CLI by ID/path), the multi-selection
    // checkbox state is irrelevant: we compress exactly the requested images.
    if (effectiveTargets.isEmpty() && multiSelectionEnabled && this->cImageModel->checkedItemsCount() == 0) {
        if (this->cliMode) {
            this->cliErrorCode = "NO_IMAGES_SELECTED";
            this->cliErrorMessage = tr("No images selected");
            return;
        }
        QCaesiumMessageBox msgBox(this);
        msgBox.setText(tr("No images selected"));
        msgBox.setInformativeText(tr("Please check at least one image to compress."));
        msgBox.addButton(tr("Ok"), QMessageBox::AcceptRole);
        msgBox.exec();
        return;
    }

    if (!QSettings().value("preferences/general/multithreading", true).toBool()) {
        QThreadPool::globalInstance()->setMaxThreadCount(1);
    } else {
        int maxThreads = QSettings().value("preferences/general/multithreading_max_threads", QThread::idealThreadCount()).toInt();
        QThreadPool::globalInstance()->setMaxThreadCount(maxThreads);
    }
    QThreadPool::globalInstance()->setThreadPriority(QSettings().value("preferences/general/threads_priority", QThread::NormalPriority).value<QThread::Priority>());

    this->compressionWatcher = new QFutureWatcher<void>();
    connect(this->compressionWatcher, &QFutureWatcherBase::finished, this, &MainWindow::compressionFinished);
    
    connect(this->compressionWatcher, &QFutureWatcherBase::finished, [this] {
        
        size_t rowCount = this->cImageModel->rowCount();
    
        for(size_t i = 0; i < rowCount; ++i) {
            this->cImageModel->emitDataChanged(i);
        }
    });

    connect(this->compressionWatcher, &QFutureWatcherBase::progressValueChanged, this->cImageModel, &CImageTreeModel::emitDataChanged);
    connect(this->compressionWatcher, &QFutureWatcherBase::progressValueChanged, this, &MainWindow::updateCompressionProgressLabel);

    ui->cancelCompression_Button->show();
    ui->compression_ProgressBar->show();
    ui->compressionProgress_Label->show();

    ui->cancelCompression_Button->setEnabled(true);
    const int progressMax = effectiveTargets.isEmpty() ? totalImages : static_cast<int>(effectiveTargets.size());
    ui->compressionProgress_Label->setText(tr("Compressing...") + QString(" (%1/%2)").arg("0", QString::number(progressMax)));
    ui->compression_ProgressBar->setMinimum(0);
    ui->compression_ProgressBar->setMaximum(progressMax);
    connect(this->compressionWatcher, &QFutureWatcherBase::progressValueChanged, ui->compression_ProgressBar, &QProgressBar::setValue);
    connect(ui->cancelCompression_Button, &QPushButton::clicked, this, &MainWindow::compressionCanceled);

    CompressionOptions compressionOptions = this->getCompressionOptions(rootFolder);

    auto* root = this->cImageModel->getRootItem();
    if (!effectiveTargets.isEmpty()) {
        this->compressionWatcher->setFuture(root->compressTargets(effectiveTargets, compressionOptions, onlyFailed));
    } else if (onlyFailed) {
        this->compressionWatcher->setFuture(root->compressOnlyFailed(compressionOptions));
    } else {
        this->compressionWatcher->setFuture(root->compress(compressionOptions));
    }

    if (!effectiveTargets.isEmpty()) {
        compressionSummary.totalImages = effectiveTargets.size();
        compressionSummary.totalUncompressedSize = 0;
        for (CImage* image : effectiveTargets) {
            compressionSummary.totalUncompressedSize += static_cast<double>(image->getOriginalSize());
        }
    } else {
        compressionSummary.totalImages = multiSelectionEnabled ? this->cImageModel->checkedItemsCount() : this->cImageModel->rowCount();
        compressionSummary.totalUncompressedSize = multiSelectionEnabled ? this->cImageModel->checkedOriginalItemsSize() : this->cImageModel->originalItemsSize();
    }
    compressionSummary.totalCompressedSize = 0;
    compressionSummary.elapsedTime = 0;

    this->toggleUIEnabled(false);

    compressionTimer.start();
}

CompressionOptions MainWindow::getCompressionOptions(const QString& rootFolder) const
{
    FileDatesOutputOption datesMap = {
        ui->keepCreationDate_CheckBox->isChecked(),
        ui->keepLastModifiedDate_CheckBox->isChecked(),
        ui->keepLastAccessDate_CheckBox->isChecked()
    };

    MaxOutputSize maxOutputSize {
        static_cast<MaxOutputSizeUnit>(ui->maxOutputSizeUnit_ComboBox->currentIndex()),
        static_cast<size_t>(ui->maxOutputSize_SpinBox->value())
    };

    CompressionOptions compressionOptions = {
        ui->outputFolder_LineEdit->text(),
        rootFolder,
        ui->outputSuffix_LineEdit->text(),
        ui->format_ComboBox->currentIndex(),
        ui->lossless_CheckBox->isChecked(),
        ui->keepMetadata_CheckBox->isChecked(),
        ui->keepStructure_CheckBox->isChecked(),
        ui->fitTo_ComboBox->currentIndex() != ResizeMode::NO_RESIZE,
        ui->fitTo_ComboBox->currentIndex(),
        ui->width_SpinBox->value(),
        ui->height_SpinBox->value(),
        ui->edge_SpinBox->value(),
        ui->doNotEnlarge_CheckBox->isChecked(),
        ui->sameOutputFolderAsInput_CheckBox->isChecked(),
        ui->skipIfBigger_CheckBox->isChecked(),
        ui->moveOriginalFile_CheckBox->isChecked(),
        ui->moveOriginalFile_ComboBox->currentIndex(),
        qBound(ui->JPEGQuality_Slider->value(), 1, 100),
        ui->JPEGChromaSubsampling_ComboBox->currentData(Qt::UserRole).toInt(),
        ui->JPEGProgressive_CheckBox->isChecked(),
        qBound(ui->PNGQuality_Slider->value(), 0, 100),
        qBound(ui->PNGOptimizationLevel_Slider->value(), 1, 6),
        qBound(ui->WebPQuality_Slider->value(), 1, 100),
        ui->TIFFCompressionMethod_ComboBox->currentIndex(),
        qBound(ui->TIFFDeflateLevel_Slider->value() * 3, 1, 9),
        ui->keepDates_CheckBox->checkState() != Qt::Unchecked,
        datesMap,
        static_cast<CompressionMode>(ui->compressionMode_ComboBox->currentIndex()),
        maxOutputSize,
        ui->cacheOriginals_CheckBox->isChecked(),
        ui->deleteOriginalFormatFile_CheckBox->isChecked(),
        QSettings().value("preferences/general/transparency_fill_color", "#FFFFFF").toString(),
        ui->syncMetaFile_CheckBox->isChecked(),
        ui->syncSpineFile_CheckBox->isChecked(),
        ui->syncFontAtlasFiles_CheckBox->isChecked(),
        ui->TGACompressionMethod_ComboBox->currentIndex() == 0,
    };

    return compressionOptions;
}

void MainWindow::on_actionAdd_folder_triggered()
{
    return MainWindow::triggerImportFolder();
}

void MainWindow::on_removeFiles_Button_clicked()
{
    this->removeFiles(false);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!this->cliQuitInProgress && !this->explicitExitRequested
        && QSettings().value("preferences/general/close_to_tray", false).toBool()
        && this->minimizeToTray()) {
        event->ignore();
        return;
    }
    if (QSettings().value("preferences/general/prompt_before_exit", false).toBool() && !this->cliQuitInProgress) {
        QCaesiumMessageBox exitPrompt(this);
        exitPrompt.setObjectName("exitPrompt");
        exitPrompt.setText(tr("Do you really want to quit?"));

        auto exitButton = exitPrompt.addButton(tr("Yes"), QMessageBox::YesRole);
        auto trayButton = exitPrompt.addButton(tr("Minimize to tray"), QMessageBox::ActionRole);
        trayButton->setEnabled(QSystemTrayIcon::isSystemTrayAvailable());
        auto rejectButton = exitPrompt.addButton(tr("Cancel"), QMessageBox::RejectRole);
        exitPrompt.setDefaultButton(exitButton);
        exitPrompt.setEscapeButton(rejectButton);

        exitPrompt.exec();

        if (exitPrompt.clickedButton() == trayButton) {
            this->minimizeToTray();
        }
        if (exitPrompt.clickedButton() != exitButton) {
            event->ignore();
            return;
        }
    }

    this->writeSettings();
    MainWindow::clearCache();
    this->previewWatcher->waitForFinished();
    qInfo() << "---- Closing application ----";
    Logger::closeLogFile();
    Logger::cleanOldLogs();
    event->accept();
}

void MainWindow::on_outputFolderBrowse_Button_clicked()
{
    QString directoryPath = QFileDialog::getExistingDirectory(this, tr("Select output folder..."),
        ui->outputFolder_LineEdit->text(),
        QFileDialog::ShowDirsOnly);

    if (!directoryPath.isEmpty()) {
        ui->outputFolder_LineEdit->setText(directoryPath);

        QSettings().setValue("compression_options/output/output_folder", directoryPath);
    }
}

void MainWindow::on_outputSuffix_LineEdit_textChanged(const QString& arg1)
{
    QSettings().setValue("compression_options/output/output_suffix", arg1);
}

void MainWindow::imageList_selectionChanged()
{
    this->selectedIndexes = ui->imageList_TreeView->selectionModel()->selectedIndexes();
    this->selectedCount = this->selectedIndexes.count() / CIMAGE_COLUMNS_SIZE;
    ui->originalImageSize_Label->clear();
    ui->compressedImageSize_Label->clear();
    ui->originalImageInfo_Label->clear();
    ui->compressedImageInfo_Label->clear();
    ui->actionPreview->setEnabled(this->selectedCount == 1 && !this->previewWatcher->isRunning());
    if (this->isItemRemovalRunning) {
        return;
    }
    ui->actionRemove->setDisabled(this->selectedCount == 0);
    ui->removeFiles_Button->setDisabled(this->selectedCount == 0);
    ui->actionShow_original_in_file_manager->setEnabled(this->selectedCount == 1);
    ui->actionShow_compressed_in_file_manager->setEnabled(this->selectedCount == 1);
    this->updateRestoreActionState();

    if (this->selectedCount == 0) {
        ui->preview_GraphicsView->removePixmap();
        ui->previewCompressed_GraphicsView->removePixmap();
        return;
    }

    auto currentIndex = this->selectedIndexes.at(0);

    if (currentIndex.row() == -1) {
        return;
    }

    bool autoPreview = QSettings().value("mainwindow/auto_preview", false).toBool();
    this->previewImage(this->proxyModel->mapToSource(currentIndex), autoPreview);
}

void MainWindow::compressionFinished()
{
    this->cImageModel->getRootItem()->setCompressionCanceled(false);
    if (ui->imageList_TreeView->selectionModel()->selectedRows().count() > 0) {
        this->previewImage(this->proxyModel->mapToSource(ui->imageList_TreeView->selectionModel()->selectedRows().at(0)));
    }

    compressionSummary.totalCompressedSize = QSettings().value("preferences/general/enable_multi_selection", false).toBool()
        ? this->cImageModel->checkedCompressedItemsSize()
        : this->cImageModel->compressedItemsSize();
    compressionSummary.elapsedTime = compressionTimer.isValid() ? compressionTimer.elapsed() : 0;

    if (QSettings().value("preferences/general/send_usage_reports", true).toBool()) {
        this->networkOperations->sendUsageReport(compressionSummary);
    }

    qInfo() << "Compression ended successfully.\nTotal images:" << compressionSummary.totalImages
            << "\nUncompressed size:" << toHumanSize(compressionSummary.totalUncompressedSize)
            << "\nCompressed size:" << toHumanSize(compressionSummary.totalCompressedSize)
            << "\nElapsed time:" << compressionSummary.elapsedTime << "ms";

    QString title = tr("Compression finished!");
    QString saved = toHumanSize(compressionSummary.totalUncompressedSize - compressionSummary.totalCompressedSize);
    QString savedPerc = QString::number(round((compressionSummary.totalUncompressedSize - compressionSummary.totalCompressedSize) / compressionSummary.totalUncompressedSize * 100));

    ui->cancelCompression_Button->hide();
    ui->compression_ProgressBar->hide();
    ui->compressionProgress_Label->hide();
    this->toggleUIEnabled(true);

    // Refresh the status bar so the compressed-size total reflects the results.
    this->cModelItemsChanged();
    this->updateRestoreActionState();

    PostCompressionAction postCompressionAction = static_cast<PostCompressionAction>(QSettings().value("preferences/general/post_compression_action", 0).toInt());
    if (postCompressionAction != PostCompressionAction::NO_ACTION) {
        PostCompressionActions::runAction(postCompressionAction, ui->outputFolder_LineEdit->text());
        return;
    }

    if (this->suppressSummaryDialog) {
        // CLI-initiated compression: no tray popup and no modal summary dialog,
        // so the process never blocks waiting for user input.
        this->suppressSummaryDialog = false;
        return;
    }

    this->trayIcon->showMessage(title, tr("You just saved %1!").arg(saved), QSystemTrayIcon::NoIcon);
    if (!QSettings().value("preferences/general/skip_compression_dialogs").toBool()) {
        QCaesiumMessageBox compressionSummaryDialog;
        compressionSummaryDialog.setText(title);
        compressionSummaryDialog.setInformativeText(tr("Total files: %1\nOriginal size: %2\nCompressed size: %3\nSaved: %4 (%5%)")
                .arg(QString::number(compressionSummary.totalImages),
                    toHumanSize(compressionSummary.totalUncompressedSize),
                    toHumanSize(compressionSummary.totalCompressedSize),
                    saved,
                    savedPerc));
        compressionSummaryDialog.addButton(tr("Ok"), QMessageBox::AcceptRole);
        compressionSummaryDialog.exec();
    }
}

void MainWindow::on_actionRemove_triggered()
{
    this->removeFiles(false);
}

void MainWindow::on_actionClear_triggered()
{
    this->removeFiles(true);
}

void MainWindow::dropFinished(const QStringList& filePaths)
{
    QString baseFolder = Importer::getRootFolder(filePaths);
    this->importFiles(filePaths, baseFolder);
}

void MainWindow::on_fitTo_ComboBox_currentIndexChanged(int index) const
{
    switch (index) {
    default:
    case ResizeMode::NO_RESIZE:
        ui->resize_Frame->setHidden(true);
        break;
    case ResizeMode::DIMENSIONS:
        ui->resize_Frame->setHidden(false);
        ui->edge_Label->hide();
        ui->edge_SpinBox->hide();
        ui->width_Label->show();
        ui->width_SpinBox->show();
        ui->width_SpinBox->setSuffix(tr("px"));
        ui->width_SpinBox->setMaximum(99999);
        ui->height_Label->show();
        ui->height_SpinBox->show();
        ui->height_SpinBox->setSuffix(tr("px"));
        ui->height_SpinBox->setMaximum(99999);
        ui->keepAspectRatio_CheckBox->setDisabled(true);
        break;
    case ResizeMode::PERCENTAGE:
        ui->resize_Frame->setHidden(false);
        ui->edge_Label->hide();
        ui->edge_SpinBox->hide();
        ui->width_Label->show();
        ui->width_SpinBox->show();
        ui->width_SpinBox->setSuffix(tr("%"));
        ui->width_SpinBox->setMaximum(ui->doNotEnlarge_CheckBox->isChecked() ? 100 : 999);
        ui->height_Label->show();
        ui->height_SpinBox->show();
        ui->height_SpinBox->setSuffix(tr("%"));
        ui->height_SpinBox->setMaximum(ui->doNotEnlarge_CheckBox->isChecked() ? 100 : 999);
        ui->keepAspectRatio_CheckBox->setEnabled(true);
        break;
    case ResizeMode::SHORT_EDGE:
    case ResizeMode::LONG_EDGE:
        ui->resize_Frame->setHidden(false);
        ui->edge_Label->show();
        ui->edge_SpinBox->show();
        ui->width_Label->hide();
        ui->width_SpinBox->hide();
        ui->height_Label->hide();
        ui->height_SpinBox->hide();
        ui->keepAspectRatio_CheckBox->setDisabled(true);
        break;
    case ResizeMode::FIXED_WIDTH:
        ui->resize_Frame->setHidden(false);
        ui->edge_Label->hide();
        ui->edge_SpinBox->hide();
        ui->width_Label->show();
        ui->width_SpinBox->show();
        ui->width_SpinBox->setSuffix(tr("px"));
        ui->width_SpinBox->setMaximum(99999);
        ui->height_Label->hide();
        ui->height_SpinBox->hide();
        ui->height_SpinBox->setSuffix(tr("px"));
        ui->keepAspectRatio_CheckBox->setEnabled(false);
        break;
    case ResizeMode::FIXED_HEIGHT:
        ui->resize_Frame->setHidden(false);
        ui->edge_Label->hide();
        ui->edge_SpinBox->hide();
        ui->width_Label->hide();
        ui->width_SpinBox->hide();
        ui->width_SpinBox->setSuffix(tr("px"));
        ui->height_Label->show();
        ui->height_SpinBox->show();
        ui->height_SpinBox->setSuffix(tr("px"));
        ui->height_SpinBox->setMaximum(99999);
        ui->keepAspectRatio_CheckBox->setEnabled(false);
        break;
    }

    QSettings().setValue("compression_options/resize/fit_to", index);
    this->toggleLosslessWarningVisible();
    this->refreshPreview();
}

void MainWindow::on_width_SpinBox_valueChanged(int value) const
{
    if (ui->fitTo_ComboBox->currentIndex() == ResizeMode::PERCENTAGE && ui->keepAspectRatio_CheckBox->isChecked()) {
        ui->height_SpinBox->setValue(value);
    }
    QSettings().setValue("compression_options/resize/width", value);
    // Preview is refreshed on editingFinished (Enter / focus-out), not on every
    // keystroke, to avoid lag while typing.
}

void MainWindow::on_height_SpinBox_valueChanged(int value) const
{
    if (ui->fitTo_ComboBox->currentIndex() == ResizeMode::PERCENTAGE && ui->keepAspectRatio_CheckBox->isChecked()) {
        ui->width_SpinBox->setValue(value);
    }
    QSettings().setValue("compression_options/resize/height", value);
}

void MainWindow::on_edge_SpinBox_valueChanged(int value) const
{
    QSettings().setValue("compression_options/resize/size", value);
}

void MainWindow::on_keepAspectRatio_CheckBox_toggled(bool checked) const
{
    if (ui->fitTo_ComboBox->currentIndex() == ResizeMode::PERCENTAGE && checked) {
        ui->height_SpinBox->setValue(ui->width_SpinBox->value());
    }
    QSettings().setValue("compression_options/resize/keep_aspect_ratio", checked);
    this->refreshPreview();
}

void MainWindow::on_doNotEnlarge_CheckBox_toggled(bool checked) const
{
    if (ui->fitTo_ComboBox->currentIndex() == ResizeMode::PERCENTAGE && checked) {
        ui->width_SpinBox->setMaximum(100);
        ui->height_SpinBox->setMaximum(100);
    } else {
        int maximum = ui->fitTo_ComboBox->currentIndex() == ResizeMode::PERCENTAGE ? 999 : 99999;
        ui->width_SpinBox->setMaximum(maximum);
        ui->height_SpinBox->setMaximum(maximum);
    }
    QSettings().setValue("compression_options/resize/do_not_enlarge", checked);
    this->refreshPreview();
}

void MainWindow::refreshPreview() const
{
    // Debounce: coalesce rapid parameter changes (dragging a slider, typing,
    // stepping a spinbox) into a single preview regeneration after a short pause.
    if (this->previewRefreshTimer != nullptr) {
        this->previewRefreshTimer->start();
    }
}

void MainWindow::doRefreshPreview() const
{
    // Keep the "resize enabled" flag in sync so the preview/compression use the
    // current mode, then force a live runtime preview of the selected image.
    QSettings().setValue("compression_options/resize/resize", ui->fitTo_ComboBox->currentIndex() != ResizeMode::NO_RESIZE);

    if (!QSettings().value("mainwindow/previews_visible", false).toBool()) {
        return;
    }
    if (this->selectedCount != 1 || this->selectedIndexes.isEmpty()) {
        return;
    }
    auto currentIndex = this->selectedIndexes.at(0);
    if (currentIndex.row() == -1) {
        return;
    }

    auto sourceIndex = this->proxyModel->mapToSource(currentIndex);
    // Drop any stale cached preview for the new settings, then regenerate.
    CImage* cImage = this->cImageModel->getRootItem()->children().at(sourceIndex.row())->getCImage();
    QString previewPath = cImage->getTemporaryPreviewFullPath();
    if (!previewPath.isEmpty() && QFile::exists(previewPath)) {
        QFile::remove(previewPath);
    }
    this->previewImage(sourceIndex, true);
}


void MainWindow::on_actionSelect_All_triggered() const
{
    ui->imageList_TreeView->selectAll();
}

void MainWindow::on_sameOutputFolderAsInput_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/output/same_folder_as_input", checked);
}

void MainWindow::on_keepStructure_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/compression/keep_structure", checked);
}

void MainWindow::on_lossless_CheckBox_toggled(bool checked) const
{
    QSettings().setValue("compression_options/compression/lossless", checked);

    ui->JPEGOptions_Frame->setEnabled(!checked);
    ui->WebPOptions_Frame->setEnabled(!checked);

    ui->PNGQuality_Label->setEnabled(!checked);
    ui->PNGQuality_SpinBox->setEnabled(!checked);
    ui->PNGQuality_Slider->setEnabled(!checked);
    ui->PNGOptimizationLevel_Label->setEnabled(checked);
    ui->PNGOptimizationLevel_SpinBox->setEnabled(checked);
    ui->PNGOptimizationLevel_Slider->setEnabled(checked);

    this->toggleLosslessWarningVisible();
}

void MainWindow::on_keepMetadata_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/compression/keep_metadata", checked);
}

void MainWindow::onJPEGQualityValueChanged(int value) const
{
    if (ui->JPEGQuality_Slider->value() != value) {
        ui->JPEGQuality_Slider->setValue(value);
    }

    if (ui->JPEGQuality_SpinBox->value() != value) {
        ui->JPEGQuality_SpinBox->setValue(value);
    }
    QSettings().setValue("compression_options/compression/jpeg_quality", value);
}

void MainWindow::onPNGQualityValueChanged(int value) const
{
    if (ui->PNGQuality_SpinBox->value() != value) {
        ui->PNGQuality_SpinBox->setValue(value);
    }
    if (ui->PNGQuality_Slider->value() != value) {
        ui->PNGQuality_Slider->setValue(value);
    }
    QSettings().setValue("compression_options/compression/png_quality", value);
}

void MainWindow::onWebPQualityValueChanged(int value) const
{
    if (ui->WebPQuality_SpinBox->value() != value) {
        ui->WebPQuality_SpinBox->setValue(value);
    }
    if (ui->WebPQuality_Slider->value() != value) {
        ui->WebPQuality_Slider->setValue(value);
    }
    QSettings().setValue("compression_options/compression/webp_quality", value);
}

void MainWindow::cModelItemsChanged() const
{
    int itemsCount = this->cImageModel->rowCount();
    QString humanItemsCount = QString::number(itemsCount);
    QString totalSize = toHumanSize(this->cImageModel->originalItemsSize());
    QString totalCompressedSize = toHumanSize(this->cImageModel->compressedItemsSize());
    ui->statusbar->showMessage(humanItemsCount + " " + tr("images in list")
        + " | " + tr("Original:") + " " + totalSize
        + " | " + tr("Compressed:") + " " + totalCompressedSize);

    ui->removeFiles_Button->setEnabled(itemsCount > 0 && this->selectedCount > 0);
    ui->actionRemove->setEnabled(itemsCount > 0 && this->selectedCount > 0);
    ui->actionClear->setDisabled(itemsCount == 0);
    ui->actionSelect_All->setDisabled(itemsCount == 0);
    ui->compress_Button->setDisabled(itemsCount == 0);
    ui->actionCompress->setDisabled(itemsCount == 0);

    ui->actionShow_original_in_file_manager->setDisabled(itemsCount == 0);
    ui->actionShow_compressed_in_file_manager->setDisabled(itemsCount == 0);

    this->updateHeaderCheckState();
}

void MainWindow::initUpdater()
{
    if (QSettings().value("preferences/general/check_updates_at_startup", true).toBool()) {
        // Silent check on startup: only prompt if a newer version is found.
        this->checkForUpdates(true);
    }
}

void MainWindow::checkForUpdates(bool silent) const
{
    QString appcastUrl = "https://raw.githubusercontent.com/zhai23/caesium-image-compressor/refs/heads/main/appcast.xml";

    auto* manager = new QNetworkAccessManager(const_cast<MainWindow*>(this));
    QNetworkRequest request { QUrl(appcastUrl) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager, silent]() {
        reply->deleteLater();
        manager->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            if (!silent) {
                QCaesiumMessageBox msgBox;
                msgBox.setText(tr("Could not check for updates."));
                msgBox.setInformativeText(reply->errorString());
                msgBox.addButton(tr("Ok"), QMessageBox::AcceptRole);
                msgBox.exec();
            }
            return;
        }

        const QString xml = QString::fromUtf8(reply->readAll());

        // Parse the latest version and download URL from the appcast.
        QString latestVersion;
        QString downloadUrl;
        QRegularExpression versionRe(R"RX(sparkle:version\s*=\s*"([^"]+)")RX");
        QRegularExpression urlRe(R"RX(url\s*=\s*"([^"]+)")RX");
        QRegularExpressionMatch vm = versionRe.match(xml);
        QRegularExpressionMatch um = urlRe.match(xml);
        if (vm.hasMatch()) {
            latestVersion = vm.captured(1).trimmed();
        }
        if (um.hasMatch()) {
            downloadUrl = um.captured(1).trimmed();
        }

        if (latestVersion.isEmpty()) {
            if (!silent) {
                QCaesiumMessageBox msgBox;
                msgBox.setText(tr("Could not check for updates."));
                msgBox.addButton(tr("Ok"), QMessageBox::AcceptRole);
                msgBox.exec();
            }
            return;
        }

        QString currentVersion = QCoreApplication::applicationVersion();
        if (isVersionNewer(latestVersion, currentVersion)) {
            QCaesiumMessageBox msgBox;
            msgBox.setText(tr("A new version is available!"));
            msgBox.setInformativeText(tr("Version %1 is available (you have %2). Do you want to download it?")
                    .arg(latestVersion, currentVersion));
            auto* yesButton = msgBox.addButton(tr("Download"), QMessageBox::YesRole);
            msgBox.addButton(tr("Not now"), QMessageBox::NoRole);
            msgBox.exec();
            if (msgBox.clickedButton() == yesButton && !downloadUrl.isEmpty()) {
                QDesktopServices::openUrl(QUrl(downloadUrl, QUrl::TolerantMode));
            }
        } else if (!silent) {
            QCaesiumMessageBox msgBox;
            msgBox.setText(tr("You are up to date!"));
            msgBox.setInformativeText(tr("Version %1 is the latest available.").arg(currentVersion));
            msgBox.addButton(tr("Ok"), QMessageBox::AcceptRole);
            msgBox.exec();
        }
    });
}

bool MainWindow::isVersionNewer(const QString& candidate, const QString& current)
{
    const QStringList c = candidate.split('.');
    const QStringList u = current.split('.');
    int n = qMax(c.size(), u.size());
    for (int i = 0; i < n; ++i) {
        int cv = i < c.size() ? c.at(i).toInt() : 0;
        int uv = i < u.size() ? u.at(i).toInt() : 0;
        if (cv != uv) {
            return cv > uv;
        }
    }
    return false;
}

void MainWindow::on_actionShow_previews_toggled(bool toggled) const
{
    ui->main_VSplitter->setChildrenCollapsible(!toggled);
    ui->main_VSplitter->handle(1)->setEnabled(toggled);
    QSettings().setValue("mainwindow/previews_visible", toggled);

    if (!toggled) {
        QSettings().setValue("mainwindow/main_splitter_sizes", QVariant::fromValue<QList<int>>(ui->main_VSplitter->sizes()));
        ui->main_VSplitter->setSizes(QList<int>({ 500, 0 }));
    } else {
        ui->main_VSplitter->setSizes(QSettings().value("mainwindow/main_splitter_sizes", QVariant::fromValue<QList<int>>({ 500, 250 })).value<QList<int>>());
        if (this->selectedCount == 1) {
            bool autoPreview = QSettings().value("mainwindow/auto_preview", false).toBool();
            this->previewImage(this->proxyModel->mapToSource(this->selectedIndexes.at(0)), autoPreview);
        }
    }
}

void MainWindow::showListContextMenu(const QPoint& pos) const
{
    // Update action states right before showing, so Restore/etc. reflect the
    // current state immediately (aboutToShow alone can lag in some cases).
    this->listContextMenuAboutToShow();
    this->listContextMenu->exec(ui->imageList_TreeView->viewport()->mapToGlobal(pos));
}

void MainWindow::on_actionPreferences_triggered()
{
    auto* preferencesDialog = new PreferencesDialog(this);
    preferencesDialog->setModal(true);

    preferencesDialog->show();
}

void MainWindow::keepDatesButtonGroupClicked() const
{
    int checkedCount = 0;
    int totalButtons = static_cast<int>(this->keepDatesButtonGroup->buttons().count());
    Qt::CheckState mainCheckboxState = Qt::PartiallyChecked;
    foreach (QAbstractButton* button, this->keepDatesButtonGroup->buttons()) {
        checkedCount += button->isChecked() ? 1 : 0;
    }

    if (checkedCount == totalButtons) {
        mainCheckboxState = Qt::Checked;
    } else if (checkedCount == 0) {
        mainCheckboxState = Qt::Unchecked;
    }

    ui->keepDates_CheckBox->setCheckState(mainCheckboxState);

    QSettings().setValue("compression_options/output/keep_creation_date", ui->keepCreationDate_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/keep_last_modified_date", ui->keepLastModifiedDate_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/keep_last_access_date", ui->keepLastAccessDate_CheckBox->isChecked());
}

void MainWindow::on_keepDates_CheckBox_clicked() const
{
    if (ui->keepDates_CheckBox->checkState() == Qt::PartiallyChecked) {
        return;
    }

    bool mainChecked = ui->keepDates_CheckBox->checkState() == Qt::Checked;
    foreach (QAbstractButton* button, this->keepDatesButtonGroup->buttons()) {
        button->setChecked(mainChecked);
    }

    QSettings().setValue("compression_options/output/keep_creation_date", ui->keepCreationDate_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/keep_last_modified_date", ui->keepLastModifiedDate_CheckBox->isChecked());
    QSettings().setValue("compression_options/output/keep_last_access_date", ui->keepLastAccessDate_CheckBox->isChecked());
}

void MainWindow::on_keepDates_CheckBox_stateChanged(int state)
{
    QSettings().setValue("compression_options/output/keep_dates", state);
}

void MainWindow::on_actionShow_original_in_file_manager_triggered() const
{
    if (this->selectedCount != 1) {
        return;
    }

    auto currentIndex = this->proxyModel->mapToSource(this->selectedIndexes.at(0));
    auto cImage = this->cImageModel->getRootItem()->children().at(currentIndex.row())->getCImage();

    // The file that actually lives on disk in the original location is the
    // compressed output (same folder + base name, possibly a new extension after
    // a format conversion). Show that if it exists; otherwise fall back to the
    // untouched original.
    QString path = cImage->getCompressedFullPath();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        path = cImage->getFullPath();
    }
    showFileInNativeFileManager(path, QFileInfo(path).absolutePath());
}

void MainWindow::on_actionShow_compressed_in_file_manager_triggered() const
{
    if (this->selectedCount != 1) {
        return;
    }

    auto currentIndex = this->proxyModel->mapToSource(this->selectedIndexes.at(0));
    auto cImage = this->cImageModel->getRootItem()->children().at(currentIndex.row())->getCImage();
    if (cImage->getCompressedFullPath().isEmpty()) {
        return;
    }
    showFileInNativeFileManager(cImage->getCompressedFullPath(), cImage->getCompressedDirectory());
}

void MainWindow::on_actionRestore_triggered()
{
    if (this->selectedCount < 1) {
        return;
    }

    // Collect unique source rows (selectedIndexes has one entry per column).
    QSet<int> rows;
    for (const QModelIndex& idx : this->selectedIndexes) {
        rows.insert(this->proxyModel->mapToSource(idx).row());
    }

    int restoredCount = 0;
    int failedCount = 0;
    for (int row : rows) {
        auto* cImage = this->cImageModel->getRootItem()->children().at(row)->getCImage();
        if (cImage->restoreFromCache(ui->syncFontAtlasFiles_CheckBox->isChecked())) {
            this->cImageModel->emitDataChanged(row);
            restoredCount++;
        } else {
            failedCount++;
        }
    }

    // Refresh the status bar totals and the currently shown preview.
    this->cModelItemsChanged();
    this->updateRestoreActionState();
    if (this->selectedCount == 1 && !this->selectedIndexes.isEmpty()) {
        this->previewImage(this->proxyModel->mapToSource(this->selectedIndexes.at(0)));
    }

    if (failedCount > 0 && restoredCount == 0) {
        QCaesiumMessageBox msgBox(this);
        msgBox.setText(tr("Nothing to restore"));
        msgBox.setInformativeText(tr("The selected images have no cached original to restore from."));
        msgBox.addButton(tr("Ok"), QMessageBox::AcceptRole);
        msgBox.exec();
    }
}

void MainWindow::listContextMenuAboutToShow() const
{
    if (this->selectedCount < 1) {
        return;
    }

    auto currentIndex = this->proxyModel->mapToSource(this->selectedIndexes.at(0));
    auto cImage = this->cImageModel->getRootItem()->children().at(currentIndex.row())->getCImage();
    ui->actionShow_compressed_in_file_manager->setDisabled(cImage->getCompressedFullPath().isEmpty());

    this->updateRestoreActionState();
}

void MainWindow::updateRestoreActionState() const
{
    // Enable Restore only if at least one selected image has a cached original
    // AND is currently in a compressed/converted state (i.e. there is something
    // to restore). After a restore the compressed info is cleared, so it disables.
    bool anyRestorable = false;
    if (this->selectedCount >= 1) {
        QSet<int> rows;
        for (const QModelIndex& idx : this->selectedIndexes) {
            rows.insert(this->proxyModel->mapToSource(idx).row());
        }
        for (int row : rows) {
            auto* img = this->cImageModel->getRootItem()->children().at(row)->getCImage();
            if (!img->getCompressedFullPath().isEmpty()
                && QFileInfo::exists(img->getCachedOriginalPath())) {
                anyRestorable = true;
                break;
            }
        }
    }
    ui->actionRestore->setEnabled(anyRestorable);
}

void MainWindow::showPreview(int index) const
{
    ImagePreview imagePreview = previewWatcher->resultAt(index);
    if (index == 0) {
        ui->preview_GraphicsView->setLoading(false);
        ui->originalImageSize_Label->setLoading(false);
        ui->preview_GraphicsView->showPixmap(imagePreview.image);
        ui->preview_GraphicsView->fitInView(ui->preview_GraphicsView->scene()->itemsBoundingRect(), Qt::KeepAspectRatio);
        ui->preview_GraphicsView->show();
        if (imagePreview.fileInfo.exists()) {
            QString resolution = QString("%1x%2").arg(imagePreview.image.width()).arg(imagePreview.image.height());
            ui->originalImageInfo_Label->setText(QString("%1 %2").arg(imagePreview.format, resolution));
            ui->originalImageSize_Label->setText(toHumanSize(static_cast<double>(imagePreview.fileInfo.size())));
        } else {
            ui->originalImageInfo_Label->clear();
            ui->originalImageSize_Label->setText(tr("File not found"));
        }
    }

    if (index == 1) {
        QString sizeText = tr("File not found");
        QString infoText;
        if (imagePreview.fileInfo.exists()) {
            auto originalSize = static_cast<double>(imagePreview.originalSize);
            auto currentSize = static_cast<double>(imagePreview.fileInfo.size());
            QString icon = "=";
            QString color = "#14b8a6";
            if (currentSize < originalSize) {
                icon = "↓";
                color = "#22c55e";
            } else if (currentSize > originalSize) {
                icon = "↑";
                color = "#ef4444";
            }
            QString ratio = QString::number(round(-100 + (currentSize / originalSize * 100))) + "%";
            QString resolution = QString("%1x%2").arg(imagePreview.image.width()).arg(imagePreview.image.height());
            infoText = QString("%1 %2").arg(imagePreview.format, resolution);
            if (imagePreview.isOnFlyPreview) {
                infoText += " (" + tr("Preview") + ")";
            }
            sizeText = QString("<span style=\" color:%1;\">%2</span> %3 (%4)").arg(color, icon, toHumanSize(currentSize), ratio);
        }
        ui->previewCompressed_GraphicsView->setLoading(false);
        ui->compressedImageSize_Label->setLoading(false);
        ui->previewCompressed_GraphicsView->showPixmap(imagePreview.image);
        ui->compressedImageInfo_Label->setText(infoText);
        ui->compressedImageSize_Label->setText(sizeText);
        ui->previewCompressed_GraphicsView->fitInView(ui->previewCompressed_GraphicsView->scene()->itemsBoundingRect(), Qt::KeepAspectRatio);
        ui->previewCompressed_GraphicsView->show();
    }
}
void MainWindow::compressionCanceled() const
{
    qInfo() << "Compression canceled by user.";
    ui->cancelCompression_Button->setEnabled(false);
    ui->compressionProgress_Label->setText(tr("Finishing..."));
    ui->compression_ProgressBar->setMinimum(0);
    ui->compression_ProgressBar->setMaximum(0);
    ui->compression_ProgressBar->setValue(0);
    this->cImageModel->getRootItem()->setCompressionCanceled(true);
    this->compressionWatcher->cancel();
}

void MainWindow::listSortChanged(int logicalIndex, Qt::SortOrder order)
{
    QSettings().setValue("mainwindow/list_view/sort_column_index", logicalIndex);
    QSettings().setValue("mainwindow/list_view/sort_column_order", order);
}

void MainWindow::onHeaderCheckStateToggled(Qt::CheckState state) const
{
    this->cImageModel->setAllChecked(state == Qt::Checked);
}

void MainWindow::updateHeaderCheckState() const
{
    auto* header = qobject_cast<QCheckBoxHeaderView*>(ui->imageList_TreeView->header());
    if (header == nullptr) {
        return;
    }

    int total = this->cImageModel->getRootItem()->childCount();
    int checked = this->cImageModel->checkedItemsCount();

    Qt::CheckState state;
    if (total == 0 || checked == 0) {
        state = Qt::Unchecked;
    } else if (checked == total) {
        state = Qt::Checked;
    } else {
        state = Qt::PartiallyChecked;
    }
    header->setCheckState(state);
}

void MainWindow::on_actionCompress_triggered()
{
    this->startCompression();
}

void MainWindow::on_actionToolbarIcons_only_triggered() const
{
    ui->actionToolbarIcons_and_Text->setChecked(false);
    ui->actionToolbarHide->setChecked(false);
    ui->actionToolbarIcons_only->setChecked(true);
    ui->toolBar->setVisible(true);
    ui->toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    QSettings().setValue("mainwindow/toolbar/visible", true);
    QSettings().setValue("mainwindow/toolbar/button_style", Qt::ToolButtonIconOnly);
}

void MainWindow::on_actionToolbarIcons_and_Text_triggered() const
{
    ui->actionToolbarIcons_only->setChecked(false);
    ui->actionToolbarHide->setChecked(false);
    ui->actionToolbarIcons_and_Text->setChecked(true);
    ui->toolBar->setVisible(true);
    ui->toolBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    QSettings().setValue("mainwindow/toolbar/visible", true);
    QSettings().setValue("mainwindow/toolbar/button_style", Qt::ToolButtonTextUnderIcon);
}

void MainWindow::on_actionToolbarHide_triggered() const
{
    ui->actionToolbarIcons_only->setChecked(false);
    ui->actionToolbarIcons_and_Text->setChecked(false);
    ui->actionToolbarHide->setChecked(true);
    ui->toolBar->setVisible(false);
    QSettings().setValue("mainwindow/toolbar/visible", false);
}

void MainWindow::toggleUIEnabled(bool enabled) const
{
    ui->toolBar->setEnabled(enabled);
    ui->parameters_TabWidget->setEnabled(enabled);
    ui->listActions_Frame->setEnabled(enabled);
    listContextMenu->setEnabled(enabled);
    ui->menuBar->setEnabled(enabled);
}

void MainWindow::updateCompressionProgressLabel(int value) const
{
    ui->compressionProgress_Label->setText(tr("Compressing...") + QString(" (%1/%2)").arg(QString::number(value), QString::number(ui->compression_ProgressBar->maximum())));
}

void MainWindow::on_actionAuto_preview_toggled(bool toggled)
{
    QSettings().setValue("mainwindow/auto_preview", toggled);
}

void MainWindow::previewFinished() const
{
    ui->preview_GraphicsView->setZoomEnabled(true);
    ui->previewCompressed_GraphicsView->setZoomEnabled(true);
    ui->actionPreview->setEnabled(this->selectedCount == 1);
}

void MainWindow::clearCache()
{
    QDir cacheDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    if (!cacheDir.exists()) {
        return;
    }

    // Remove the cached originals subfolder (used by "Cache originals for re-compression").
    QDir originalsDir(cacheDir.absoluteFilePath("originals"));
    if (originalsDir.exists()) {
        originalsDir.removeRecursively();
    }

    foreach (QString cacheFile, cacheDir.entryList()) {
        cacheDir.remove(cacheFile);
    }
}

void MainWindow::previewCanceled() const
{
    ui->preview_GraphicsView->setLoading(false);
    ui->originalImageSize_Label->setLoading(false);
    ui->previewCompressed_GraphicsView->setLoading(false);
    ui->compressedImageSize_Label->setLoading(false);
}

void MainWindow::on_actionPreview_triggered() const
{
    if (this->selectedCount != 1) {
        return;
    }

    auto currentIndex = this->selectedIndexes.at(0);

    if (currentIndex.row() == -1) {
        return;
    }

    this->previewImage(this->proxyModel->mapToSource(currentIndex), true);
}

void MainWindow::on_skipIfBigger_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/output/skip_if_bigger", checked);
}

void MainWindow::on_cacheOriginals_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/output/cache_originals", checked);
}

void MainWindow::on_deleteOriginalFormatFile_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/output/delete_original_format_file", checked);
}

void MainWindow::on_syncMetaFile_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/output/sync_meta_file", checked);
}

void MainWindow::on_syncSpineFile_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/output/sync_spine_file", checked);
}

void MainWindow::on_syncFontAtlasFiles_CheckBox_toggled(bool checked)
{
    QSettings().setValue("compression_options/output/sync_font_atlas_files", checked);
}

void MainWindow::outputFormatIndexChanged(int index) const
{
    QSettings().setValue("compression_options/output/format", index);
    this->toggleLosslessWarningVisible();
}

void MainWindow::importFromArgs(const QStringList& args)
{
    bool scanSubfolders = QSettings().value("preferences/general/import_subfolders", true).toBool();
    QStringList filesList = Importer::scanList(args, scanSubfolders);
    if (filesList.isEmpty()) {
        return;
    }

    ImportFromArgsMethod argsBehaviour = static_cast<ImportFromArgsMethod>(QSettings().value("preferences/general/args_behaviour", 0).toInt());
    QString baseFolder = Importer::getRootFolder(args);
    this->importFiles(filesList, baseFolder);
    if (argsBehaviour == IMPORT_AND_COMPRESS) {
        this->startCompression();
    }
}

void MainWindow::moveOriginalFileToggled(bool checked) const
{
    ui->moveOriginalFile_ComboBox->setEnabled(checked);
    QSettings().setValue("compression_options/output/move_original_file", checked);
}

void MainWindow::moveOriginalFileDestinationChanged(int index)
{
    QSettings().setValue("compression_options/output/move_original_file_destination", index);
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
    }
}

QTranslator* MainWindow::getTranslator() const
{
    return translator;
}

void MainWindow::onMaxOutputSizeChanged(int value) const
{
    QSettings().setValue("compression_options/compression/max_output_size", ui->maxOutputSize_SpinBox->value());
}

void MainWindow::onMaxOutputSizeUnitChanged(int value) const
{
    if (value == MaxOutputSizeUnit::MAX_OUTPUT_PERCENTAGE && ui->maxOutputSize_SpinBox->value() > 100) {
        ui->maxOutputSize_SpinBox->setValue(100);
    }
    QSettings().setValue("compression_options/compression/max_output_size_unit", ui->maxOutputSizeUnit_ComboBox->currentIndex());
}

void MainWindow::toggleLosslessWarningVisible() const
{
    bool showLosslessWarning = ui->lossless_CheckBox->isChecked()
        && (ui->format_ComboBox->currentIndex() != 0 || ui->fitTo_ComboBox->currentIndex() != ResizeMode::NO_RESIZE)
        && ui->compressionMode_ComboBox->currentIndex() == QUALITY;

    ui->losslessWarning_Button->setVisible(showLosslessWarning);
}

void MainWindow::onCompressionModeChanged(int value) const
{
    QSettings().setValue("compression_options/compression/mode", ui->compressionMode_ComboBox->currentIndex());

    // Keep TGA last among the format groups, and available in size mode.
    if (value == QUALITY) {
        ui->verticalLayout_7->insertWidget(ui->verticalLayout_7->indexOf(ui->qualityContainer_Widget), ui->TGAOptions_Widget);
    } else {
        ui->verticalLayout_4->insertWidget(0, ui->TGAOptions_Widget);
    }
    ui->TGAOptions_Widget->show();

    this->toggleLosslessWarningVisible();
}

void MainWindow::onAdvancedImportTriggered()
{
    const auto advancedImportDialog = new AdvancedImportDialog();
    connect(advancedImportDialog, &AdvancedImportDialog::importTriggered, [this](const QStringList& fileList) {
        if (fileList.isEmpty()) {
            return;
        }
        QString baseFolder = Importer::getRootFolder(fileList);
        this->importFiles(fileList, baseFolder);
    });

    advancedImportDialog->exec();
}

void MainWindow::onPNGOptimizationLevelChanged(int value) const
{
    if (ui->PNGOptimizationLevel_SpinBox->value() != value) {
        ui->PNGOptimizationLevel_SpinBox->setValue(value);
    }
    if (ui->PNGOptimizationLevel_Slider->value() != value) {
        ui->PNGOptimizationLevel_Slider->setValue(value);
    }

    QSettings().setValue("compression_options/compression/png_optimization_level", value);
}

void MainWindow::onTIFFCompressionMethodChanged(int index) const
{
    ui->TIFFDeflateLevelContainer_Widget->setEnabled(index == 2);
    QSettings().setValue("compression_options/compression/tiff_method", index);
}

void MainWindow::onTIFFDeflateLevelChanged(int value) const
{
    QSettings().setValue("compression_options/compression/tiff_deflate_level", value);
    this->updateTIFFDeflateLevelLabel(value);
}

void MainWindow::updateTIFFDeflateLevelLabel(int value) const
{
    QString text;
    if (value <= 1) {
        text = tr("Fast");
    } else if (value >= 3) {
        text = tr("Best");
    } else {
        text = tr("Balanced");
    }
    ui->TIFFDeflateLevelValue_Label->setText(text);
}

void MainWindow::onJPEGChromaSubsamplingChanged() const
{
    QSettings().setValue("compression_options/compression/jpeg_chroma_subsampling", ui->JPEGChromaSubsampling_ComboBox->currentData(Qt::UserRole));
}

void MainWindow::setupChromaSubsamplingComboBox() const
{
    auto chromaSubsamplings = getChromaSubsamplingOptions();
    auto iterator = QMapIterator<int, QString>(chromaSubsamplings);
    while (iterator.hasNext()) {
        auto chromaSubsamplingOption = iterator.next();
        ui->JPEGChromaSubsampling_ComboBox->addItem(chromaSubsamplingOption.value(), chromaSubsamplingOption.key());
    }
}

void MainWindow::setupCompressButton()
{
    auto recompressAction = new QAction(tr("Recompress failed"));
    connect(recompressAction, &QAction::triggered, this, &MainWindow::recompressFailed);
    this->ui->compress_Button->addAction(recompressAction);
}

namespace {

// Icon of a search option toggle: the label in the normal text color while the
// option is on, grayed out under a red slash while it is off.
QIcon searchOptionIcon(const QString& label, bool on, const QWidget* reference)
{
    const qreal pixelRatio = reference->devicePixelRatioF();
    QPixmap pixmap(qRound(24 * pixelRatio), qRound(24 * pixelRatio));
    pixmap.setDevicePixelRatio(pixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(pixelRatio, pixelRatio);
    QFont font = reference->font();
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(reference->palette().color(on ? QPalette::Active : QPalette::Disabled, QPalette::Text));
    painter.drawText(QRect(0, 0, 24, 24), Qt::AlignCenter, label);
    if (!on) {
        // Semi-transparent so the grayed out label stays readable underneath.
        QPen slash(QColor(220, 45, 45, 150));
        slash.setWidthF(2.0);
        slash.setCapStyle(Qt::RoundCap);
        painter.setPen(slash);
        painter.drawLine(5, 19, 19, 5);
    }
    return QIcon(pixmap);
}

} // namespace

void MainWindow::setupSearchBox()
{
    // Built in code (and not in the .ui) so the search box sits right behind
    // the toolbar actions and so the option buttons can draw their own icons.
    auto* searchWidget = new QWidget(ui->toolBar);
    searchWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* searchLayout = new QHBoxLayout(searchWidget);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(0);

    this->searchLineEdit = new QLineEdit(searchWidget);
    this->searchLineEdit->setObjectName("searchLineEdit");
    this->searchLineEdit->setPlaceholderText(tr("Search file names..."));
    this->searchLineEdit->setToolTip(tr("Filter the list by the original file name (fuzzy: matches any part of it)."));
    this->searchLineEdit->setClearButtonEnabled(true);
    this->searchLineEdit->setFixedWidth(200);
    searchLayout->addWidget(this->searchLineEdit);

    this->searchRegexToolButton = new QToolButton(searchWidget);
    this->searchRegexToolButton->setObjectName("searchRegexToolButton");
    this->searchRegexToolButton->setCheckable(true);
    this->searchRegexToolButton->setAutoRaise(true);
    this->searchRegexToolButton->setIconSize(QSize(24, 24));
    this->searchRegexToolButton->setToolTip(tr("Use a regular expression instead of a fuzzy match."));
    searchLayout->addWidget(this->searchRegexToolButton);

    this->searchCaseToolButton = new QToolButton(searchWidget);
    this->searchCaseToolButton->setObjectName("searchCaseToolButton");
    this->searchCaseToolButton->setCheckable(true);
    this->searchCaseToolButton->setAutoRaise(true);
    this->searchCaseToolButton->setIconSize(QSize(24, 24));
    this->searchCaseToolButton->setToolTip(tr("Distinguish upper and lower case when searching."));
    searchLayout->addWidget(this->searchCaseToolButton);

    auto updateSearchOptionIcons = [this]() {
        this->searchRegexToolButton->setIcon(searchOptionIcon(".*", this->searchRegexToolButton->isChecked(), this->searchRegexToolButton));
        this->searchCaseToolButton->setIcon(searchOptionIcon("Aa", this->searchCaseToolButton->isChecked(), this->searchCaseToolButton));
    };
    updateSearchOptionIcons();

    ui->toolBar->addSeparator();
    ui->toolBar->addWidget(searchWidget);

    connect(this->searchLineEdit, &QLineEdit::textChanged, this, &MainWindow::applySearchFilter);
    connect(this->searchRegexToolButton, &QToolButton::toggled, this, &MainWindow::applySearchFilter);
    connect(this->searchCaseToolButton, &QToolButton::toggled, this, &MainWindow::applySearchFilter);
    connect(this->searchRegexToolButton, &QToolButton::toggled, this, [updateSearchOptionIcons](bool) {
        updateSearchOptionIcons();
    });
    connect(this->searchCaseToolButton, &QToolButton::toggled, this, [updateSearchOptionIcons](bool) {
        updateSearchOptionIcons();
    });
    connect(this->searchRegexToolButton, &QToolButton::toggled, this, [](bool checked) {
        QSettings().setValue("mainwindow/search/regex_mode", checked);
    });
    connect(this->searchCaseToolButton, &QToolButton::toggled, this, [](bool checked) {
        QSettings().setValue("mainwindow/search/case_sensitive", checked);
    });

    // Ctrl+F jumps to the search box, ESC clears it while it has the focus.
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, &MainWindow::focusSearchBox);
    auto* clearShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this->searchLineEdit);
    clearShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(clearShortcut, &QShortcut::activated, this->searchLineEdit, &QLineEdit::clear);
}

void MainWindow::applySearchFilter() const
{
    this->proxyModel->setSearchFilter(this->searchLineEdit->text(),
        this->searchRegexToolButton->isChecked(),
        this->searchCaseToolButton->isChecked());
}

void MainWindow::focusSearchBox() const
{
    this->searchLineEdit->setFocus();
    this->searchLineEdit->selectAll();
}

QList<CImage*> MainWindow::visibleImages() const
{
    QList<CImage*> images;
    const bool multiSelectionEnabled = QSettings().value("preferences/general/enable_multi_selection", false).toBool();
    const int rows = this->proxyModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex sourceIndex = this->proxyModel->mapToSource(this->proxyModel->index(row, CImageColumns::NAME_COLUMN));
        auto* item = static_cast<CImageTreeItem*>(sourceIndex.internalPointer());
        if (item == nullptr) {
            continue;
        }
        if (multiSelectionEnabled && !item->isChecked()) {
            continue;
        }
        images.append(item->getCImage());
    }
    return images;
}

void MainWindow::onJPEGOptionsVisibilityChanged(bool visible)
{
    QSettings().setValue("mainwindow/compression/jpeg_options_visible", visible);
}
void MainWindow::onPNGOptionsVisibilityChanged(bool visible)
{
    QSettings().setValue("mainwindow/compression/png_options_visible", visible);
}
void MainWindow::onWebPOptionsVisibilityChanged(bool visible)
{
    QSettings().setValue("mainwindow/compression/webp_options_visible", visible);
}
void MainWindow::onTIFFOptionsVisibilityChanged(bool visible)
{
    QSettings().setValue("mainwindow/compression/tiff_options_visible", visible);
}
void MainWindow::onJPEGProgressiveToggled(bool checked)
{
    QSettings().setValue("compression_options/compression/jpeg_progressive", checked);
}

void MainWindow::recompressFailed()
{
    this->startCompression(true);
}

void MainWindow::installCompressionOptionsEventFilter() const
{
    // TODO This is not super optimal, we should not disable scrolling on the widgets when we are not scrolling the ScrollArea
    ui->JPEGQuality_Slider->installEventFilter(new QSliderScrollFilter());
    ui->JPEGQuality_SpinBox->installEventFilter(new QSliderScrollFilter());
    ui->JPEGChromaSubsampling_ComboBox->installEventFilter(new QSliderScrollFilter());

    ui->PNGQuality_Slider->installEventFilter(new QSliderScrollFilter());
    ui->PNGQuality_SpinBox->installEventFilter(new QSliderScrollFilter());
    ui->PNGOptimizationLevel_Slider->installEventFilter(new QSliderScrollFilter());
    ui->PNGOptimizationLevel_SpinBox->installEventFilter(new QSliderScrollFilter());

    ui->WebPQuality_Slider->installEventFilter(new QSliderScrollFilter());
    ui->WebPQuality_SpinBox->installEventFilter(new QSliderScrollFilter());

    ui->TIFFDeflateLevel_Slider->installEventFilter(new QSliderScrollFilter());
    ui->TIFFCompressionMethod_ComboBox->installEventFilter(new QSliderScrollFilter());
    ui->TGACompressionMethod_ComboBox->installEventFilter(new QSliderScrollFilter());
}

// ---------------------------------------------------------------------------
// CLI command handlers. These run on the main thread and reuse the same code
// paths as the UI, so any change is reflected in the interface immediately.
// ---------------------------------------------------------------------------

namespace {

QString cliStatusToString(CImageStatus status)
{
    switch (status) {
    case CImageStatus::UNCOMPRESSED:
        return "uncompressed";
    case CImageStatus::COMPRESSING:
        return "compressing";
    case CImageStatus::COMPRESSED:
        return "compressed";
    case CImageStatus::ERROR:
        return "error";
    case CImageStatus::WARNING:
        return "warning";
    }
    return "unknown";
}

QString cliFormatIndexToString(int index)
{
    switch (index) {
    case 1:
        return "jpg";
    case 2:
        return "png";
    case 3:
        return "webp";
    case 4:
        return "tiff";
    case 5:
        return "tga";
    default:
        return "same";
    }
}

int cliFormatStringToIndex(const QString& value)
{
    if (value.compare("jpg", Qt::CaseInsensitive) == 0 || value.compare("jpeg", Qt::CaseInsensitive) == 0) {
        return 1;
    }
    if (value.compare("png", Qt::CaseInsensitive) == 0) {
        return 2;
    }
    if (value.compare("webp", Qt::CaseInsensitive) == 0) {
        return 3;
    }
    if (value.compare("tiff", Qt::CaseInsensitive) == 0) {
        return 4;
    }
    if (value.compare("tga", Qt::CaseInsensitive) == 0) {
        return 5;
    }
    return 0; // same as input
}

QString cliTiffMethodIndexToString(int index)
{
    switch (index) {
    case 1:
        return "lzw";
    case 2:
        return "deflate";
    case 3:
        return "packbits";
    default:
        return "uncompressed";
    }
}

int cliTiffMethodStringToIndex(const QString& value)
{
    if (value.compare("lzw", Qt::CaseInsensitive) == 0) {
        return 1;
    }
    if (value.compare("deflate", Qt::CaseInsensitive) == 0) {
        return 2;
    }
    if (value.compare("packbits", Qt::CaseInsensitive) == 0) {
        return 3;
    }
    return 0;
}

QString cliUnitIndexToString(int index)
{
    switch (index) {
    case 1:
        return "kb";
    case 2:
        return "mb";
    case 3:
        return "percentage";
    default:
        return "bytes";
    }
}

int cliUnitStringToIndex(const QString& value)
{
    if (value.compare("kb", Qt::CaseInsensitive) == 0) {
        return 1;
    }
    if (value.compare("mb", Qt::CaseInsensitive) == 0) {
        return 2;
    }
    if (value.compare("percentage", Qt::CaseInsensitive) == 0 || value.compare("percent", Qt::CaseInsensitive) == 0) {
        return 3;
    }
    return 0;
}

QString cliFitIndexToString(int index)
{
    switch (index) {
    case ResizeMode::DIMENSIONS:
        return "dimensions";
    case ResizeMode::PERCENTAGE:
        return "percentage";
    case ResizeMode::SHORT_EDGE:
        return "short_edge";
    case ResizeMode::LONG_EDGE:
        return "long_edge";
    case ResizeMode::FIXED_WIDTH:
        return "fixed_width";
    case ResizeMode::FIXED_HEIGHT:
        return "fixed_height";
    default:
        return "none";
    }
}

int cliFitStringToIndex(const QString& value)
{
    if (value.compare("none", Qt::CaseInsensitive) == 0) {
        return ResizeMode::NO_RESIZE;
    }
    if (value.compare("dimensions", Qt::CaseInsensitive) == 0) {
        return ResizeMode::DIMENSIONS;
    }
    if (value.compare("percentage", Qt::CaseInsensitive) == 0) {
        return ResizeMode::PERCENTAGE;
    }
    if (value.compare("short_edge", Qt::CaseInsensitive) == 0) {
        return ResizeMode::SHORT_EDGE;
    }
    if (value.compare("long_edge", Qt::CaseInsensitive) == 0) {
        return ResizeMode::LONG_EDGE;
    }
    if (value.compare("fixed_width", Qt::CaseInsensitive) == 0) {
        return ResizeMode::FIXED_WIDTH;
    }
    if (value.compare("fixed_height", Qt::CaseInsensitive) == 0) {
        return ResizeMode::FIXED_HEIGHT;
    }
    // Unknown values must be reported as invalid instead of silently falling
    // back to "no resize", so callers can surface a clear error to the user.
    return -1;
}

int cliValueToInt(const QJsonValue& value, int fallback = 0)
{
    if (value.isDouble()) {
        return value.toInt();
    }
    if (value.isString()) {
        return value.toString().toInt();
    }
    return fallback;
}

bool cliValueToBool(const QJsonValue& value, bool fallback = false)
{
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isString()) {
        const QString s = value.toString().trimmed().toLower();
        return s == "true" || s == "1" || s == "yes" || s == "on";
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    return fallback;
}

// Strict integer parse: accepts a JSON double or a numeric string. Unlike
// cliValueToInt it sets ok to false for non-numeric or non-integral values so
// callers can reject the input instead of silently using a fallback.
int cliValueToIntStrict(const QJsonValue& value, bool& ok)
{
    ok = true;
    if (value.isString()) {
        const QString s = value.toString().trimmed();
        return s.toInt(&ok);
    }
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (d < -2147483648.0 || d > 2147483647.0) {
            ok = false;
            return 0;
        }
        const int v = static_cast<int>(d);
        if (static_cast<double>(v) != d) {
            ok = false;
            return 0;
        }
        return v;
    }
    ok = false;
    return 0;
}

} // namespace

QStringList MainWindow::stringListFromJson(const QJsonValue& value)
{
    QStringList list;
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue& v : array) {
            if (v.isString()) {
                list << v.toString();
            }
        }
    } else if (value.isString()) {
        list << value.toString();
    }
    return list;
}

bool MainWindow::isCompressionRunning() const
{
    return this->compressionWatcher != nullptr && this->compressionWatcher->isRunning();
}

QJsonObject MainWindow::imageToJson(CImage* cImage) const
{
    QJsonObject obj;
    obj["id"] = cImage->getCliId();
    obj["path"] = cImage->getFullPath();
    obj["name"] = cImage->getFileName();
    obj["format"] = cImage->getFormat();
    obj["status"] = cliStatusToString(cImage->getStatus());
    obj["original_size"] = static_cast<double>(cImage->getOriginalSize());
    obj["compressed_size"] = static_cast<double>(cImage->getCompressedSize());
    obj["ratio"] = cImage->getRatio();
    obj["saved"] = cImage->getFormattedSavedRatio();
    obj["compressed_path"] = cImage->getCompressedFullPath();
    return obj;
}

CliResponse MainWindow::handleCliRequest(const QString& method, const QJsonObject& params)
{
    if (method == "ping") {
        return { true, {}, {}, {} };
    }
    if (method == "import") {
        return this->cliImport(params);
    }
    if (method == "remove") {
        return this->cliRemove(params);
    }
    if (method == "clear") {
        return this->cliClear(params);
    }
    if (method == "list") {
        return this->cliList(params);
    }
    if (method == "status") {
        return this->cliStatus(params);
    }
    if (method == "compress") {
        return this->cliCompress(params);
    }
    if (method == "cancel") {
        return this->cliCancel(params);
    }
    if (method == "select") {
        return this->cliSelect(params);
    }
    if (method == "restore") {
        return this->cliRestore(params);
    }
    if (method == "preview") {
        return this->cliPreview(params);
    }
    if (method == "options.get") {
        return this->cliGetOptions(params);
    }
    if (method == "options.set") {
        return this->cliSetOptions(params);
    }
    if (method == "settings.get") {
        return this->cliGetSettings(params);
    }
    if (method == "settings.set") {
        return this->cliSetSettings(params);
    }
    if (method == "show") {
        return this->cliShow(params);
    }
    if (method == "quit") {
        return this->cliQuit(params);
    }
    return { false, "UNKNOWN_METHOD", tr("Unknown command: %1").arg(method), {} };
}

CliResponse MainWindow::cliImport(const QJsonObject& params)
{
    if (this->isCompressionRunning()) {
        return { false, "COMPRESS_IN_PROGRESS", tr("Compression is in progress. Wait for it to finish or cancel it first."), {} };
    }
    const QStringList paths = stringListFromJson(params.value("paths"));
    if (paths.isEmpty()) {
        return { false, "NO_PATHS", tr("No paths specified."), {} };
    }
    const bool scanSubfolders = QSettings().value("preferences/general/import_subfolders", true).toBool();
    const QStringList filesList = Importer::scanList(paths, scanSubfolders);
    if (filesList.isEmpty()) {
        return { false, "NO_FILES", tr("No supported image files found in the given paths."), {} };
    }
    const int before = this->cImageModel->rowCount();
    const QString baseFolder = Importer::getRootFolder(paths);
    this->importFiles(filesList, baseFolder, false);
    const int added = this->cImageModel->rowCount() - before;
    QJsonObject result;
    result["added"] = added;
    result["scanned"] = filesList.count();
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliRemove(const QJsonObject& params)
{
    if (this->isCompressionRunning()) {
        return { false, "COMPRESS_IN_PROGRESS", tr("Compression is in progress."), {} };
    }
    const QStringList paths = stringListFromJson(params.value("paths"));
    QList<int> ids;
    if (params.value("ids").isArray()) {
        const QJsonArray idArr = params.value("ids").toArray();
        for (const QJsonValue& v : idArr) {
            if (v.isDouble() || v.isString()) {
                ids << v.toVariant().toInt();
            }
        }
    }
    if (paths.isEmpty() && ids.isEmpty()) {
        return { false, "NO_PATHS", tr("No paths or IDs specified."), {} };
    }
    auto* root = this->cImageModel->getRootItem();
    int removed = 0;
    for (const QString& path : paths) {
        const QString normalized = QFileInfo(path).absoluteFilePath();
        for (int i = root->childCount() - 1; i >= 0; --i) {
            CImage* img = root->children().at(i)->getCImage();
            if (img->getFullPath() == normalized) {
                this->updateFolderMap(normalized, -1);
                img->removeCachedOriginal();
                this->cImageModel->removeRows(i, 1, QModelIndex());
                removed++;
                break;
            }
        }
    }
    for (int id : ids) {
        for (int i = root->childCount() - 1; i >= 0; --i) {
            CImage* img = root->children().at(i)->getCImage();
            if (img->getCliId() == id) {
                this->updateFolderMap(img->getFullPath(), -1);
                img->removeCachedOriginal();
                this->cImageModel->removeRows(i, 1, QModelIndex());
                removed++;
                break;
            }
        }
    }
    this->previewWatcher->cancel();
    this->cModelItemsChanged();
    this->updateRestoreActionState();
    QJsonObject result;
    result["removed"] = removed;
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliClear(const QJsonObject&)
{
    if (this->isCompressionRunning()) {
        return { false, "COMPRESS_IN_PROGRESS", tr("Compression is in progress."), {} };
    }
    auto* root = this->cImageModel->getRootItem();
    const int count = root->childCount();
    for (int i = count - 1; i >= 0; --i) {
        CImage* img = root->children().at(i)->getCImage();
        this->updateFolderMap(img->getFullPath(), -1);
        img->removeCachedOriginal();
    }
    if (count > 0) {
        this->cImageModel->removeRows(0, count, QModelIndex());
    }
    this->previewWatcher->cancel();
    this->cModelItemsChanged();
    QJsonObject result;
    result["cleared"] = count;
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliList(const QJsonObject&)
{
    auto* root = this->cImageModel->getRootItem();
    QJsonArray items;
    for (int i = 0; i < root->childCount(); ++i) {
        auto* item = root->children().at(i);
        QJsonObject obj = this->imageToJson(item->getCImage());
        obj["checked"] = item->isChecked();
        items.append(obj);
    }
    QJsonObject result;
    result["items"] = items;
    result["count"] = items.size();
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliStatus(const QJsonObject&)
{
    auto* root = this->cImageModel->getRootItem();
    QJsonObject result;
    const bool running = this->isCompressionRunning();
    result["compressing"] = running;
    result["images"] = this->cImageModel->rowCount();
    result["selected"] = this->cImageModel->checkedItemsCount();
    result["multi_selection"] = QSettings().value("preferences/general/enable_multi_selection", false).toBool();
    // Total sizes for the whole list, matching the GUI status bar.
    result["total_original_size"] = this->cImageModel->originalItemsSize();
    result["total_compressed_size"] = this->cImageModel->compressedItemsSize();

    QJsonObject progress;
    if (running && this->compressionWatcher != nullptr) {
        progress["current"] = this->compressionWatcher->progressValue();
        progress["total"] = this->compressionWatcher->progressMaximum();
    } else {
        progress["current"] = 0;
        progress["total"] = 0;
    }
    result["progress"] = progress;

    QJsonArray items;
    int compressed = 0;
    int error = 0;
    int uncompressed = 0;
    int compressing = 0;
    int warning = 0;
    for (int i = 0; i < root->childCount(); ++i) {
        auto* item = root->children().at(i);
        CImage* img = item->getCImage();
        QJsonObject obj = this->imageToJson(img);
        obj["checked"] = item->isChecked();
        items.append(obj);
        switch (img->getStatus()) {
        case CImageStatus::COMPRESSED:
            compressed++;
            break;
        case CImageStatus::ERROR:
            error++;
            break;
        case CImageStatus::COMPRESSING:
            compressing++;
            break;
        case CImageStatus::WARNING:
            warning++;
            break;
        default:
            uncompressed++;
            break;
        }
    }
    result["items"] = items;
    QJsonObject counts;
    counts["compressed"] = compressed;
    counts["error"] = error;
    counts["uncompressed"] = uncompressed;
    counts["compressing"] = compressing;
    counts["warning"] = warning;
    result["counts"] = counts;
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliCompress(const QJsonObject& params)
{
    if (this->isCompressionRunning()) {
        return { false, "COMPRESS_IN_PROGRESS", tr("Compression is already running."), {} };
    }

    auto* root = this->cImageModel->getRootItem();

    // Resolve explicit targets. Paths may be files or folders, so expand them
    // the same way the importer does, then match the resulting files against
    // the images currently in the list. IDs refer to the CLI session IDs.
    const QStringList paths = stringListFromJson(params.value("paths"));
    QList<int> ids;
    if (params.value("ids").isArray()) {
        const QJsonArray idArr = params.value("ids").toArray();
        for (const QJsonValue& v : idArr) {
            if (v.isDouble() || v.isString()) {
                ids << v.toVariant().toInt();
            }
        }
    }
    const bool explicitTargets = !paths.isEmpty() || !ids.isEmpty();

    QList<CImage*> targets;
    if (!paths.isEmpty()) {
        const bool scanSubfolders = QSettings().value("preferences/general/import_subfolders", true).toBool();
        const QStringList files = Importer::scanList(paths, scanSubfolders);
        for (const QString& file : files) {
            const QString normalized = QFileInfo(file).absoluteFilePath();
            for (int i = 0; i < root->childCount(); ++i) {
                CImage* img = root->children().at(i)->getCImage();
                if (img->getFullPath() == normalized) {
                    targets << img;
                    break;
                }
            }
        }
    }
    for (int id : ids) {
        for (int i = 0; i < root->childCount(); ++i) {
            CImage* img = root->children().at(i)->getCImage();
            if (img->getCliId() == id) {
                targets << img;
                break;
            }
        }
    }

    if (explicitTargets && targets.isEmpty()) {
        return { false, "NO_MATCH", tr("None of the given paths/IDs are in the list."), {} };
    }
    if (!explicitTargets && this->cImageModel->rowCount() == 0) {
        return { false, "NO_IMAGES", tr("No images in the list. Add some first."), {} };
    }
    if (ui->outputFolder_LineEdit->text().isEmpty() && !ui->sameOutputFolderAsInput_CheckBox->isChecked()) {
        return { false, "NO_OUTPUT_FOLDER", tr("Please select an output folder first."), {} };
    }
    if (!explicitTargets
        && QSettings().value("preferences/general/enable_multi_selection", false).toBool()
        && this->cImageModel->checkedItemsCount() == 0) {
        return { false, "NO_IMAGES_SELECTED", tr("No images selected."), {} };
    }
    const bool onlyFailed = params.value("only_failed").toBool(false);
    this->cliMode = true;
    this->cliErrorCode.clear();
    this->cliErrorMessage.clear();
    this->suppressSummaryDialog = true;
    this->startCompression(onlyFailed, targets);
    this->cliMode = false;
    if (!this->cliErrorCode.isEmpty()) {
        return { false, this->cliErrorCode, this->cliErrorMessage, {} };
    }
    QJsonObject result;
    result["started"] = true;
    if (explicitTargets) {
        result["total"] = targets.size();
    } else {
        const bool multiSelectionEnabled = QSettings().value("preferences/general/enable_multi_selection", false).toBool();
        result["total"] = multiSelectionEnabled ? this->cImageModel->checkedItemsCount() : this->cImageModel->rowCount();
    }
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliCancel(const QJsonObject&)
{
    if (!this->isCompressionRunning()) {
        return { false, "NOT_COMPRESSING", tr("No compression in progress."), {} };
    }
    this->compressionCanceled();
    QJsonObject result;
    result["canceled"] = true;
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliSelect(const QJsonObject& params)
{
    if (this->isCompressionRunning()) {
        return { false, "COMPRESS_IN_PROGRESS", tr("Compression is in progress."), {} };
    }

    const bool multiSelection = QSettings().value("preferences/general/enable_multi_selection", false).toBool();
    auto* root = this->cImageModel->getRootItem();

    // Bulk select-all / select-none only makes sense with multi-selection.
    if (params.contains("all")) {
        if (multiSelection) {
            this->cImageModel->setAllChecked(cliValueToBool(params.value("all"), true));
        }
        QJsonObject result;
        result["checked"] = this->cImageModel->checkedItemsCount();
        result["total"] = this->cImageModel->rowCount();
        return { true, {}, {}, result };
    }

    const QStringList paths = stringListFromJson(params.value("paths"));
    QList<int> ids;
    if (params.value("ids").isArray()) {
        const QJsonArray idArr = params.value("ids").toArray();
        for (const QJsonValue& v : idArr) {
            if (v.isDouble() || v.isString()) {
                ids << v.toVariant().toInt();
            }
        }
    }
    if (paths.isEmpty() && ids.isEmpty()) {
        return { false, "NO_PATHS", tr("No paths or IDs specified."), {} };
    }
    const bool checked = cliValueToBool(params.value("checked"), true);

    QList<CImage*> targets;
    auto applyCheck = [&](CImage* img, int row) {
        if (multiSelection) {
            const QModelIndex idx = this->cImageModel->index(row, CImageColumns::CHECKBOX_COLUMN);
            this->cImageModel->setData(idx, checked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        }
    };

    for (const QString& path : paths) {
        const QString normalized = QFileInfo(path).absoluteFilePath();
        for (int i = 0; i < root->childCount(); ++i) {
            CImage* img = root->children().at(i)->getCImage();
            if (img->getFullPath() == normalized) {
                applyCheck(img, i);
                targets << img;
                break;
            }
        }
    }
    for (int id : ids) {
        for (int i = 0; i < root->childCount(); ++i) {
            CImage* img = root->children().at(i)->getCImage();
            if (img->getCliId() == id) {
                applyCheck(img, i);
                targets << img;
                break;
            }
        }
    }

    if (targets.isEmpty()) {
        return { false, "NO_MATCH", tr("None of the given paths/IDs are in the list."), {} };
    }

    // Locate the first matched image in the GUI so its preview shows.
    this->highlightImageInGui(targets.first());

    QJsonObject result;
    result["checked"] = this->cImageModel->checkedItemsCount();
    result["total"] = this->cImageModel->rowCount();
    result["highlighted"] = targets.first()->getCliId();
    return { true, {}, {}, result };
}

// Highlights a single image in the GUI (row selection), so it is located and
// its preview is shown. Works whether or not multi-selection is on.
void MainWindow::highlightImageInGui(CImage* img)
{
    auto* root = this->cImageModel->getRootItem();
    for (int i = 0; i < root->childCount(); ++i) {
        if (root->children().at(i)->getCImage() == img) {
            const QModelIndex srcIdx = this->cImageModel->index(i, 0);
            const QModelIndex proxyIdx = this->proxyModel->mapFromSource(srcIdx);
            if (proxyIdx.isValid() && ui->imageList_TreeView->selectionModel() != nullptr) {
                ui->imageList_TreeView->selectionModel()->setCurrentIndex(
                    proxyIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
            return;
        }
    }
}

CliResponse MainWindow::cliPreview(const QJsonObject& params)
{
    if (this->isCompressionRunning()) {
        return { false, "COMPRESS_IN_PROGRESS", tr("Compression is in progress."), {} };
    }
    // Resolve the image by ID or full path.
    CImage* target = nullptr;
    auto* root = this->cImageModel->getRootItem();
    if (params.contains("id")) {
        const int id = params.value("id").toVariant().toInt();
        for (int i = 0; i < root->childCount(); ++i) {
            if (root->children().at(i)->getCImage()->getCliId() == id) {
                target = root->children().at(i)->getCImage();
                break;
            }
        }
    } else {
        const QString path = params.value("path").toString();
        const QString normalized = QFileInfo(path).absoluteFilePath();
        for (int i = 0; i < root->childCount(); ++i) {
            if (root->children().at(i)->getCImage()->getFullPath() == normalized) {
                target = root->children().at(i)->getCImage();
                break;
            }
        }
    }
    if (target == nullptr) {
        return { false, "NO_MATCH", tr("No image with the given ID/path is in the list."), {} };
    }

    // Generate a real temporary preview using the current compression options,
    // exactly like the GUI preview pane.
    const QString rootFolder = Importer::getRootFolder(this->folderMap.keys());
    const CompressionOptions opts = this->getCompressionOptions(rootFolder);
    if (!target->preview(opts)) {
        return { false, "PREVIEW_FAILED", tr("Could not generate the preview."), {} };
    }

    const QString previewPath = target->getTemporaryPreviewFullPath();
    const double originalSize = static_cast<double>(target->getOriginalSize());
    const double previewSize = static_cast<double>(QFileInfo(previewPath).size());
    const double saved = originalSize - previewSize;
    const double savedPercent = originalSize > 0 ? (saved * 100.0 / originalSize) : 0.0;

    // Format + resolution for the source and the preview, like the GUI panes.
    const QString sourcePath = target->getCompressionSourcePath(opts.cacheOriginals);
    const QSize origRes = ImageIO::size(sourcePath);
    const QSize prevRes = ImageIO::size(previewPath);
    const QString origFormat = target->getFormat().toUpper();
    // The preview temp file is a hash-named file with no useful extension, so
    // derive the output format from the chosen compression format ("same as
    // input" keeps the original format).
    QString prevFormat = target->getFormat().toUpper();
    if (opts.format != 0) {
        prevFormat = cliFormatIndexToString(opts.format).toUpper();
    }

    QJsonObject result;
    result["id"] = target->getCliId();
    result["name"] = target->getFileName();
    result["path"] = target->getFullPath();
    result["original_format"] = origFormat;
    result["original_resolution"] = QStringLiteral("%1x%2").arg(origRes.width()).arg(origRes.height());
    result["preview_format"] = prevFormat;
    result["preview_resolution"] = QStringLiteral("%1x%2").arg(prevRes.width()).arg(prevRes.height());
    result["original_size"] = originalSize;
    result["preview_size"] = previewSize;
    result["saved"] = saved;
    result["saved_percent"] = savedPercent;
    result["preview_path"] = previewPath;
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliRestore(const QJsonObject& params)
{
    if (this->isCompressionRunning()) {
        return { false, "COMPRESS_IN_PROGRESS", tr("Compression is in progress."), {} };
    }

    auto* root = this->cImageModel->getRootItem();
    QList<CImage*> targets;
    QList<int> rows;

    const QStringList paths = stringListFromJson(params.value("paths"));
    QList<int> ids;
    if (params.value("ids").isArray()) {
        const QJsonArray idArr = params.value("ids").toArray();
        for (const QJsonValue& v : idArr) {
            if (v.isDouble() || v.isString()) {
                ids << v.toVariant().toInt();
            }
        }
    }

    if (paths.isEmpty() && ids.isEmpty()) {
        // No explicit targets: restore all currently selected (checked) images.
        // When multi-selection is off, every image is checked, so this is "all".
        for (int i = 0; i < root->childCount(); ++i) {
            auto* item = root->children().at(i);
            if (item->isChecked()) {
                targets << item->getCImage();
                rows << i;
            }
        }
    } else {
        for (const QString& path : paths) {
            const QString normalized = QFileInfo(path).absoluteFilePath();
            for (int i = 0; i < root->childCount(); ++i) {
                CImage* img = root->children().at(i)->getCImage();
                if (img->getFullPath() == normalized) {
                    targets << img;
                    rows << i;
                    break;
                }
            }
        }
        for (int id : ids) {
            for (int i = 0; i < root->childCount(); ++i) {
                CImage* img = root->children().at(i)->getCImage();
                if (img->getCliId() == id) {
                    targets << img;
                    rows << i;
                    break;
                }
            }
        }
    }

    if (targets.isEmpty()) {
        return { false, "NO_MATCH", tr("No matching images to restore."), {} };
    }

    // Deduplicate (an image may be matched by both a path and an ID).
    QList<CImage*> unique;
    QList<int> uniqueRows;
    QSet<CImage*> seen;
    for (int k = 0; k < targets.size(); ++k) {
        if (seen.contains(targets.at(k))) {
            continue;
        }
        seen.insert(targets.at(k));
        unique << targets.at(k);
        uniqueRows << rows.at(k);
    }

    int restored = 0;
    int failed = 0;
    for (int k = 0; k < unique.size(); ++k) {
        if (unique.at(k)->restoreFromCache(ui->syncFontAtlasFiles_CheckBox->isChecked())) {
            this->cImageModel->emitDataChanged(uniqueRows.at(k));
            restored++;
        } else {
            failed++;
        }
    }

    this->cModelItemsChanged();
    this->updateRestoreActionState();

    // Locate the first targeted image in the GUI so its preview shows.
    this->highlightImageInGui(unique.first());

    QJsonObject result;
    result["restored"] = restored;
    result["failed"] = failed;
    return { true, {}, {}, result };
}

QJsonObject MainWindow::currentOptionsJson() const
{
    QJsonObject o;
    o["format"] = cliFormatIndexToString(ui->format_ComboBox->currentIndex());
    o["mode"] = ui->compressionMode_ComboBox->currentIndex() == QUALITY ? "quality" : "size";
    o["lossless"] = ui->lossless_CheckBox->isChecked();
    o["keep_metadata"] = ui->keepMetadata_CheckBox->isChecked();
    o["keep_structure"] = ui->keepStructure_CheckBox->isChecked();
    o["jpeg_quality"] = ui->JPEGQuality_Slider->value();
    o["jpeg_chroma_subsampling"] = QString::number(ui->JPEGChromaSubsampling_ComboBox->currentData(Qt::UserRole).toInt());
    o["jpeg_progressive"] = ui->JPEGProgressive_CheckBox->isChecked();
    o["png_quality"] = ui->PNGQuality_Slider->value();
    o["png_optimization_level"] = ui->PNGOptimizationLevel_Slider->value();
    o["webp_quality"] = ui->WebPQuality_Slider->value();
    o["tiff_method"] = cliTiffMethodIndexToString(ui->TIFFCompressionMethod_ComboBox->currentIndex());
    o["tiff_deflate_level"] = ui->TIFFDeflateLevel_Slider->value();
    o["tga_compression"] = ui->TGACompressionMethod_ComboBox->currentIndex() == 0 ? "rle" : "none";
    o["max_output_size"] = ui->maxOutputSize_SpinBox->value();
    o["max_output_size_unit"] = cliUnitIndexToString(ui->maxOutputSizeUnit_ComboBox->currentIndex());
    o["fit_to"] = cliFitIndexToString(ui->fitTo_ComboBox->currentIndex());
    o["width"] = ui->width_SpinBox->value();
    o["height"] = ui->height_SpinBox->value();
    o["edge"] = ui->edge_SpinBox->value();
    o["keep_aspect_ratio"] = ui->keepAspectRatio_CheckBox->isChecked();
    o["do_not_enlarge"] = ui->doNotEnlarge_CheckBox->isChecked();
    o["output_folder"] = ui->outputFolder_LineEdit->text();
    o["output_suffix"] = ui->outputSuffix_LineEdit->text();
    o["same_folder_as_input"] = ui->sameOutputFolderAsInput_CheckBox->isChecked();
    o["skip_if_bigger"] = ui->skipIfBigger_CheckBox->isChecked();
    o["cache_originals"] = ui->cacheOriginals_CheckBox->isChecked();
    o["delete_original_format_file"] = ui->deleteOriginalFormatFile_CheckBox->isChecked();
    o["keep_dates"] = ui->keepDates_CheckBox->checkState() == Qt::Checked;
    o["keep_creation_date"] = ui->keepCreationDate_CheckBox->isChecked();
    o["keep_last_modified_date"] = ui->keepLastModifiedDate_CheckBox->isChecked();
    o["keep_last_access_date"] = ui->keepLastAccessDate_CheckBox->isChecked();
    return o;
}

QStringList MainWindow::applyCliOptions(const QJsonObject& options)
{
    QStringList unknown;

    if (options.contains("format")) {
        const int idx = cliFormatStringToIndex(options.value("format").toString());
        if (idx >= 0) {
            ui->format_ComboBox->setCurrentIndex(idx);
        } else {
            unknown << "format";
        }
    }
    if (options.contains("mode")) {
        const QString m = options.value("mode").toString().toLower();
        if (m == "size") {
            ui->compressionMode_ComboBox->setCurrentIndex(SIZE);
        } else if (m == "quality") {
            ui->compressionMode_ComboBox->setCurrentIndex(QUALITY);
        } else {
            unknown << "mode";
        }
    }
    if (options.contains("lossless")) {
        ui->lossless_CheckBox->setChecked(cliValueToBool(options.value("lossless")));
    }
    if (options.contains("keep_metadata")) {
        ui->keepMetadata_CheckBox->setChecked(cliValueToBool(options.value("keep_metadata")));
    }
    if (options.contains("keep_structure")) {
        ui->keepStructure_CheckBox->setChecked(cliValueToBool(options.value("keep_structure")));
    }
    if (options.contains("jpeg_quality")) {
        ui->JPEGQuality_Slider->setValue(cliValueToInt(options.value("jpeg_quality"), ui->JPEGQuality_Slider->value()));
    }
    if (options.contains("jpeg_chroma_subsampling")) {
        const int chroma = options.value("jpeg_chroma_subsampling").toVariant().toInt();
        const int idx = ui->JPEGChromaSubsampling_ComboBox->findData(chroma);
        if (idx != -1) {
            ui->JPEGChromaSubsampling_ComboBox->setCurrentIndex(idx);
        } else {
            unknown << "jpeg_chroma_subsampling";
        }
    }
    if (options.contains("jpeg_progressive")) {
        ui->JPEGProgressive_CheckBox->setChecked(cliValueToBool(options.value("jpeg_progressive")));
    }
    if (options.contains("png_quality")) {
        ui->PNGQuality_Slider->setValue(cliValueToInt(options.value("png_quality"), ui->PNGQuality_Slider->value()));
    }
    if (options.contains("png_optimization_level")) {
        ui->PNGOptimizationLevel_Slider->setValue(cliValueToInt(options.value("png_optimization_level"), ui->PNGOptimizationLevel_Slider->value()));
    }
    if (options.contains("webp_quality")) {
        ui->WebPQuality_Slider->setValue(cliValueToInt(options.value("webp_quality"), ui->WebPQuality_Slider->value()));
    }
    if (options.contains("tiff_method")) {
        const int idx = cliTiffMethodStringToIndex(options.value("tiff_method").toString());
        if (idx >= 0) {
            ui->TIFFCompressionMethod_ComboBox->setCurrentIndex(idx);
        } else {
            unknown << "tiff_method";
        }
    }
    if (options.contains("tga_compression")) {
        const QString method = options.value("tga_compression").toString().toLower();
        if (method == "rle" || method == "none") {
            ui->TGACompressionMethod_ComboBox->setCurrentIndex(method == "rle" ? 0 : 1);
        } else {
            unknown << "tga_compression";
        }
    }
    if (options.contains("tiff_deflate_level")) {
        ui->TIFFDeflateLevel_Slider->setValue(cliValueToInt(options.value("tiff_deflate_level"), ui->TIFFDeflateLevel_Slider->value()));
    }
    if (options.contains("max_output_size")) {
        ui->maxOutputSize_SpinBox->setValue(cliValueToInt(options.value("max_output_size"), ui->maxOutputSize_SpinBox->value()));
    }
    if (options.contains("max_output_size_unit")) {
        const int idx = cliUnitStringToIndex(options.value("max_output_size_unit").toString());
        if (idx >= 0) {
            ui->maxOutputSizeUnit_ComboBox->setCurrentIndex(idx);
        } else {
            unknown << "max_output_size_unit";
        }
    }
    if (options.contains("fit_to")) {
        const int idx = cliFitStringToIndex(options.value("fit_to").toString());
        if (idx >= 0) {
            ui->fitTo_ComboBox->setCurrentIndex(idx);
        } else {
            unknown << "fit_to";
        }
    }
    if (options.contains("width")) {
        bool ok = false;
        const int val = cliValueToIntStrict(options.value("width"), ok);
        if (!ok || val < ui->width_SpinBox->minimum() || val > ui->width_SpinBox->maximum()) {
            unknown << "width";
        } else {
            ui->width_SpinBox->setValue(val);
        }
    }
    if (options.contains("height")) {
        bool ok = false;
        const int val = cliValueToIntStrict(options.value("height"), ok);
        if (!ok || val < ui->height_SpinBox->minimum() || val > ui->height_SpinBox->maximum()) {
            unknown << "height";
        } else {
            ui->height_SpinBox->setValue(val);
        }
    }
    if (options.contains("edge")) {
        bool ok = false;
        const int val = cliValueToIntStrict(options.value("edge"), ok);
        if (!ok || val < ui->edge_SpinBox->minimum() || val > ui->edge_SpinBox->maximum()) {
            unknown << "edge";
        } else {
            ui->edge_SpinBox->setValue(val);
        }
    }
    if (options.contains("keep_aspect_ratio")) {
        ui->keepAspectRatio_CheckBox->setChecked(cliValueToBool(options.value("keep_aspect_ratio")));
    }
    if (options.contains("do_not_enlarge")) {
        ui->doNotEnlarge_CheckBox->setChecked(cliValueToBool(options.value("do_not_enlarge")));
    }
    if (options.contains("output_folder")) {
        const QString folder = options.value("output_folder").toString();
        ui->outputFolder_LineEdit->setText(folder);
        QSettings().setValue("compression_options/output/output_folder", folder);
    }
    if (options.contains("output_suffix")) {
        ui->outputSuffix_LineEdit->setText(options.value("output_suffix").toString());
    }
    if (options.contains("same_folder_as_input")) {
        ui->sameOutputFolderAsInput_CheckBox->setChecked(cliValueToBool(options.value("same_folder_as_input")));
    }
    if (options.contains("skip_if_bigger")) {
        ui->skipIfBigger_CheckBox->setChecked(cliValueToBool(options.value("skip_if_bigger")));
    }
    if (options.contains("cache_originals")) {
        ui->cacheOriginals_CheckBox->setChecked(cliValueToBool(options.value("cache_originals")));
    }
    if (options.contains("delete_original_format_file")) {
        ui->deleteOriginalFormatFile_CheckBox->setChecked(cliValueToBool(options.value("delete_original_format_file")));
    }
    if (options.contains("keep_dates")) {
        const bool keep = cliValueToBool(options.value("keep_dates"));
        ui->keepCreationDate_CheckBox->setChecked(keep);
        ui->keepLastModifiedDate_CheckBox->setChecked(keep);
        ui->keepLastAccessDate_CheckBox->setChecked(keep);
        ui->keepDates_CheckBox->setCheckState(keep ? Qt::Checked : Qt::Unchecked);
        QSettings().setValue("compression_options/output/keep_dates", keep ? Qt::Checked : Qt::Unchecked);
        QSettings().setValue("compression_options/output/keep_creation_date", keep);
        QSettings().setValue("compression_options/output/keep_last_modified_date", keep);
        QSettings().setValue("compression_options/output/keep_last_access_date", keep);
    }
    if (options.contains("keep_creation_date")) {
        const bool v = cliValueToBool(options.value("keep_creation_date"));
        ui->keepCreationDate_CheckBox->setChecked(v);
        QSettings().setValue("compression_options/output/keep_creation_date", v);
    }
    if (options.contains("keep_last_modified_date")) {
        const bool v = cliValueToBool(options.value("keep_last_modified_date"));
        ui->keepLastModifiedDate_CheckBox->setChecked(v);
        QSettings().setValue("compression_options/output/keep_last_modified_date", v);
    }
    if (options.contains("keep_last_access_date")) {
        const bool v = cliValueToBool(options.value("keep_last_access_date"));
        ui->keepLastAccessDate_CheckBox->setChecked(v);
        QSettings().setValue("compression_options/output/keep_last_access_date", v);
    }

    return unknown;
}

CliResponse MainWindow::cliGetOptions(const QJsonObject& params)
{
    const QJsonObject options = this->currentOptionsJson();
    if (params.contains("key")) {
        const QString key = params.value("key").toString();
        if (!options.contains(key)) {
            return { false, "UNKNOWN_KEY", tr("Unknown option key: %1").arg(key), {} };
        }
        QJsonObject result;
        result["key"] = key;
        result["value"] = options.value(key);
        return { true, {}, {}, result };
    }
    QJsonObject result;
    result["options"] = options;
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliSetOptions(const QJsonObject& params)
{
    QJsonObject options = params.value("options").toObject();
    if (options.isEmpty() && params.contains("key")) {
        options.insert(params.value("key").toString(), params.value("value"));
    }
    if (options.isEmpty()) {
        return { false, "NO_OPTIONS", tr("No options to set."), {} };
    }
    const QStringList unknown = this->applyCliOptions(options);
    QJsonObject result;
    result["applied"] = true;
    result["unknown_keys"] = QJsonArray::fromStringList(unknown);
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliGetSettings(const QJsonObject& params)
{
    QJsonObject settings = this->currentOptionsJson();

    QSettings s;
    settings["enable_multi_selection"] = s.value("preferences/general/enable_multi_selection", false).toBool();
    settings["import_subfolders"] = s.value("preferences/general/import_subfolders", true).toBool();
    settings["skip_compression_dialogs"] = s.value("preferences/general/skip_compression_dialogs", false).toBool();
    settings["prompt_before_exit"] = s.value("preferences/general/prompt_before_exit", false).toBool();
    settings["close_to_tray"] = s.value("preferences/general/close_to_tray", false).toBool();
    settings["send_usage_reports"] = s.value("preferences/general/send_usage_reports", true).toBool();
    settings["check_updates_at_startup"] = s.value("preferences/general/check_updates_at_startup", true).toBool();
    settings["multithreading"] = s.value("preferences/general/multithreading", true).toBool();
    settings["multithreading_max_threads"] = s.value("preferences/general/multithreading_max_threads", QThread::idealThreadCount()).toInt();
    settings["theme"] = s.value("preferences/general/theme", 0).toInt();
    settings["theme_variant"] = s.value("preferences/general/theme_variant", 0).toInt();
    settings["language"] = s.value("preferences/language/locale", "default").toString();
    settings["args_behaviour"] = s.value("preferences/general/args_behaviour", 0).toInt();
    settings["post_compression_action"] = s.value("preferences/general/post_compression_action", 0).toInt();
    settings["transparency_fill_color"] = s.value("preferences/general/transparency_fill_color", "#FFFFFF").toString();

    if (params.contains("key")) {
        const QString key = params.value("key").toString();
        if (!settings.contains(key)) {
            return { false, "UNKNOWN_KEY", tr("Unknown setting key: %1").arg(key), {} };
        }
        QJsonObject result;
        result["key"] = key;
        result["value"] = settings.value(key);
        return { true, {}, {}, result };
    }
    QJsonObject result;
    result["options"] = settings;
    return { true, {}, {}, result };
}

bool MainWindow::applyPreferenceSetting(const QString& key, const QJsonValue& value)
{
    QSettings s;
    if (key == QLatin1String("enable_multi_selection")) {
        this->setMultiSelectionEnabled(cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("import_subfolders")) {
        s.setValue("preferences/general/import_subfolders", cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("skip_compression_dialogs")) {
        s.setValue("preferences/general/skip_compression_dialogs", cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("prompt_before_exit")) {
        s.setValue("preferences/general/prompt_before_exit", cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("close_to_tray")) {
        s.setValue("preferences/general/close_to_tray", cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("send_usage_reports")) {
        s.setValue("preferences/general/send_usage_reports", cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("check_updates_at_startup")) {
        s.setValue("preferences/general/check_updates_at_startup", cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("multithreading")) {
        s.setValue("preferences/general/multithreading", cliValueToBool(value));
        return true;
    }
    if (key == QLatin1String("multithreading_max_threads")) {
        s.setValue("preferences/general/multithreading_max_threads", cliValueToInt(value));
        return true;
    }
    if (key == QLatin1String("theme")) {
        s.setValue("preferences/general/theme", cliValueToInt(value));
        return true;
    }
    if (key == QLatin1String("theme_variant")) {
        s.setValue("preferences/general/theme_variant", cliValueToInt(value));
        return true;
    }
    if (key == QLatin1String("language")) {
        s.setValue("preferences/language/locale", value.toString());
        return true;
    }
    if (key == QLatin1String("args_behaviour")) {
        s.setValue("preferences/general/args_behaviour", cliValueToInt(value));
        return true;
    }
    if (key == QLatin1String("post_compression_action")) {
        s.setValue("preferences/general/post_compression_action", cliValueToInt(value));
        return true;
    }
    if (key == QLatin1String("transparency_fill_color")) {
        s.setValue("preferences/general/transparency_fill_color", value.toString());
        // Regenerate the preview with the new fill colour, like the GUI colour
        // picker does.
        this->refreshPreview();
        return true;
    }
    return false;
}

bool MainWindow::isCompressionOptionKey(const QString& key) const
{
    static const QSet<QString> keys = {
        QStringLiteral("format"), QStringLiteral("mode"), QStringLiteral("lossless"),
        QStringLiteral("keep_metadata"), QStringLiteral("keep_structure"),
        QStringLiteral("jpeg_quality"), QStringLiteral("jpeg_chroma_subsampling"), QStringLiteral("jpeg_progressive"),
        QStringLiteral("png_quality"), QStringLiteral("png_optimization_level"), QStringLiteral("webp_quality"),
        QStringLiteral("tiff_method"), QStringLiteral("tiff_deflate_level"),
        QStringLiteral("tga_compression"),
        QStringLiteral("max_output_size"), QStringLiteral("max_output_size_unit"),
        QStringLiteral("fit_to"), QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("edge"),
        QStringLiteral("keep_aspect_ratio"), QStringLiteral("do_not_enlarge"),
        QStringLiteral("output_folder"), QStringLiteral("output_suffix"),
        QStringLiteral("same_folder_as_input"), QStringLiteral("skip_if_bigger"), QStringLiteral("cache_originals"),
        QStringLiteral("delete_original_format_file"),
        QStringLiteral("keep_dates"), QStringLiteral("keep_creation_date"),
        QStringLiteral("keep_last_modified_date"), QStringLiteral("keep_last_access_date"),
    };
    return keys.contains(key);
}

CliResponse MainWindow::cliSetSettings(const QJsonObject& params)
{
    QJsonObject options = params.value("options").toObject();
    if (options.isEmpty() && params.contains("key")) {
        options.insert(params.value("key").toString(), params.value("value"));
    }
    if (options.isEmpty()) {
        return { false, "NO_OPTIONS", tr("No settings to set."), {} };
    }

    // Split the keys: compression options are applied to the UI widgets; all
    // other known keys are general preferences. We cannot rely on
    // applyCliOptions' return value, because it only reports recognized keys
    // with invalid values, not unrecognized keys.
    QJsonObject compressionOpts;
    QStringList badKeys;
    for (auto it = options.begin(); it != options.end(); ++it) {
        const QString& key = it.key();
        if (this->isCompressionOptionKey(key)) {
            compressionOpts.insert(key, it.value());
        } else if (!this->applyPreferenceSetting(key, it.value())) {
            badKeys << key;
        }
    }
    const QStringList invalid = this->applyCliOptions(compressionOpts);
    badKeys += invalid;

    QJsonObject result;
    result["applied"] = true;
    result["unknown_keys"] = QJsonArray::fromStringList(badKeys);
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliShow(const QJsonObject&)
{
    this->restoreWindow();
    QJsonObject result;
    result["shown"] = true;
    return { true, {}, {}, result };
}

CliResponse MainWindow::cliQuit(const QJsonObject&)
{
    this->cliQuitInProgress = true;

    // End any visible modal dialogs (e.g. the compression summary shown after a
    // user-initiated run) so their nested event loops unwind and the app can
    // actually exit.
    const QWidgetList topLevels = QApplication::topLevelWidgets();
    for (QWidget* widget : topLevels) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog != nullptr && dialog->isVisible()) {
            dialog->done(QDialog::Rejected);
        }
    }

    this->close();

    // Ensure the event loop exits even if something else kept a loop alive.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);

    QJsonObject result;
    result["quitting"] = true;
    return { true, {}, {}, result };
}
