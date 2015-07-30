/*
 * Copyright (c) 2011 Shinnok <admin at shinnok.com>.
 * Copyright (c) 2011, 2012 Aleksey Cherepanov <aleksey.4erepanov@gmail.com>.
 * See LICENSE for details.
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "translator.h"
#include "ui_aboutwidget.h"

#include <QToolButton>
#include <QStringListModel>
#include <QFileDialog>
#include <QFile>
#include <QByteArray>
#include <QTextStream>
#include <QMessageBox>
#include <QClipboard>
#include <QThread>
#include <QTextCursor>
#include <QDesktopServices>
#include <QInputDialog>
#include <QtDebug>
#include <QDesktopWidget>
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QStandardPaths>
#endif

#define INTERVAL_PICK_CRACKED 15
#define TAB_PASSWORDS   0
#define TAB_OPTIONS     1
#define TAB_STATISTICS  2
#define TAB_SETTINGS    3
#define TAB_CONSOLE_LOG 4
#define DYNAMIC_FILTERING_HASH_LIMIT 25000
#define CONSOLE_LOG_SEPARATOR "-------------------------------------\n"

MainWindow::MainWindow(QSettings &settings)
    : QMainWindow(0),
      m_terminate(false),
      m_ui(new Ui::MainWindow),
      m_hashTable(NULL),
      m_hashTableProxy(new HashSortFilterProxyModel(this)),
      m_sessionCurrent("", &settings),
      m_johnShowTemp(NULL),
      m_settings(settings),
      m_aboutWindow(this)
{
    // UI initializations
    m_ui->setupUi(this);
    Translator &translator = Translator::getInstance();
    m_ui->comboBoxLanguageSelection->insertItems(0, translator.getListOfAvailableLanguages());
    m_ui->widgetFork->setVisible(false);
    m_ui->tableView_Hashes->setModel(m_hashTableProxy);
    m_ui->tableView_Hashes->setSortingEnabled(true);
    m_hashTableProxy->setDynamicSortFilter(false);
    m_hashTableProxy->setShowCheckedRowsOnly(m_ui->checkBoxShowOnlyCheckedHashes->isChecked());
    m_hashTableProxy->setShowCrackedRowsOnly(m_ui->checkBoxShowOnlyCheckedHashes->isChecked());
    m_ui->tableView_Hashes->sortByColumn(PasswordFileModel::USER_COL, Qt::AscendingOrder);
    // Until we get a result from john, we disable jumbo features
    m_isJumbo = false;
    setAvailabilityOfFeatures(false);
    connect(&m_johnVersionCheck, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(verifyJohnVersion()));

    // For the OS X QProgressBar issue
    // https://github.com/shinnok/johnny/issues/11
#ifdef Q_OS_OSX
    m_ui->progressBar->installEventFilter(this);

    // Also OS X doesn't display text in progress bar, add separate label for
    // showing stats
    m_progressStatsLabel = new QLabel(m_ui->contentWidget);
    m_progressStatsLabel->setAlignment(Qt::AlignHCenter);
    m_ui->contentWidgetLayout->insertWidget(1, m_progressStatsLabel);
#endif

    // We select the PASSWORDS tab
    m_ui->contentStackedWidget->setCurrentIndex(TAB_PASSWORDS);
    QActionGroup *tabSelectionGroup = new QActionGroup(this);
    tabSelectionGroup->setExclusive(true);
    foreach(QAction* actions, m_ui->tabSelectionToolBar->actions()) {
        tabSelectionGroup->addAction(actions);
    }
    m_ui->actionPasswordsTabClicked->setChecked(true);

    QMenu *filterMenu = new QMenu(m_ui->filterMenuPushButton);
    filterMenu->addAction(m_ui->actionFilterAllColumns);
    filterMenu->addAction(m_ui->actionFilterUserColumn);
    filterMenu->addAction(m_ui->actionFilterPasswordColumn);
    filterMenu->addAction(m_ui->actionFilterHashColumn);
    filterMenu->addAction(m_ui->actionFilterFormatColumn);
    filterMenu->addAction(m_ui->actionFilterGECOSColumn);
    m_ui->filterMenuPushButton->setMenu(filterMenu);
    m_ui->lineEditFilter->setToolTip(tr("Filter/Search inside the Passwords table using keywords. For large tables (> %1) press Enter/Return key to apply the filter.")
                                     .arg(DYNAMIC_FILTERING_HASH_LIMIT));
    connect(filterMenu, SIGNAL(triggered(QAction*)), this, SLOT(setFilteringColumns()));

#if QT_VERSION >= QT_VERSION_CHECK(5, 2, 0)
        m_ui->lineEditFilter->setClearButtonEnabled(true);
#elif QT_VERSION < QT_VERSION_CHECK(4,7,0)
        m_filterDirectivesLabel = new QLabel(this);
        m_filterDirectivesLabel->setText(tr("Press Enter/Return key to apply filter"));
        m_ui->passwordsPageLayout->insertWidget(1, m_filterDirectivesLabel);
        m_filterDirectivesLabel->hide();
#endif

    m_ui->widgetFilterOptions->setEnabled(false);

    m_ui->attackModeTabWidget->setCurrentWidget(m_ui->defaultModeTab);
    // Disable copy button since there is no hash_tables (UI friendly)
    m_ui->actionCopyToClipboard->setEnabled(false);
    m_ui->actionStartAttack->setEnabled(false);
    m_ui->actionIncludeSelectedHashes->setEnabled(false);
    m_ui->actionExcludeSelectedHashes->setEnabled(false);

    // Multiple sessions management menu
    m_sessionMenu = new Menu(this);
    QToolButton *sessionMenuButton = new QToolButton(this);
    sessionMenuButton->setDefaultAction(m_ui->actionOpenSession);
    sessionMenuButton->setMenu(m_sessionMenu);
    sessionMenuButton->setPopupMode(QToolButton::InstantPopup);
    sessionMenuButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_ui->mainToolBar->insertWidget(m_ui->actionStartAttack, sessionMenuButton);
    m_ui->mainToolBar->insertSeparator(m_ui->actionStartAttack);
    connect(m_ui->actionOpenSession, SIGNAL(triggered()), sessionMenuButton, SLOT(showMenu()));
    connect(m_sessionMenu, SIGNAL(triggered(QAction*)), this, SLOT(actionOpenSessionTriggered(QAction*)));

    connect(&m_johnAttack, SIGNAL(finished(int, QProcess::ExitStatus)), this,
            SLOT(showJohnFinished(int, QProcess::ExitStatus)), Qt::QueuedConnection);
    connect(&m_johnAttack, SIGNAL(started()), this,
            SLOT(showJohnStarted()), Qt::QueuedConnection);
    connect(&m_johnAttack, SIGNAL(error(QProcess::ProcessError)), this,
            SLOT(showJohnError(QProcess::ProcessError)), Qt::QueuedConnection);
    connect(&m_johnAttack, SIGNAL(readyReadStandardOutput()), this,
            SLOT(updateJohnOutput()), Qt::QueuedConnection);
    connect(&m_johnAttack, SIGNAL(readyReadStandardError()), this,
            SLOT(updateJohnOutput()), Qt::QueuedConnection);

    // We connect timer with calling for john to show us status.
    connect(&m_showTimer, SIGNAL(timeout()),
            this, SLOT(callJohnShow()));
    // We connect 'john --show' process with our object.
    connect(&m_johnShow, SIGNAL(finished(int, QProcess::ExitStatus)),
            this, SLOT(readJohnShow()));

    connect(&m_hashTypeChecker,SIGNAL(updateHashTypes(const QStringList&, const QStringList& ,const QStringList&)), this,
            SLOT(updateHashTypes(const QStringList&,const QStringList&, const QStringList&)),Qt::QueuedConnection);
    connect(&m_johnGuess, SIGNAL(finished(int,QProcess::ExitStatus)), this,
            SLOT(guessPasswordFinished(int,QProcess::ExitStatus)), Qt::QueuedConnection);
    connect(&m_johnGuess, SIGNAL(error(QProcess::ProcessError)), this,
            SLOT(showJohnError(QProcess::ProcessError)), Qt::QueuedConnection);

    // Settings changed by user
    connect(m_ui->spinBoxTimeIntervalPickCracked,SIGNAL(valueChanged(int)),this,SLOT(applyAndSaveSettings()));
    connect(m_ui->lineEditPathToJohn,SIGNAL(textEdited(QString)),this,SLOT(applyAndSaveSettings()));
    connect(m_ui->comboBoxLanguageSelection,SIGNAL(currentIndexChanged(int)),this,SLOT(applyAndSaveSettings()));

    // Action buttons
    connect(m_ui->actionOpenPassword,SIGNAL(triggered()),this,SLOT(openPasswordFile()));
    connect(m_ui->actionPauseAttack,SIGNAL(triggered()),this,SLOT(pauseAttack()));
    connect(m_ui->actionResumeAttack,SIGNAL(triggered()),this,SLOT(resumeAttack()));
    connect(m_ui->actionStartAttack,SIGNAL(triggered()),this,SLOT(startAttack()));
    connect(m_ui->pushButtonStatisticsUpdateStatus,SIGNAL(clicked()),this,SLOT(updateStatistics()));
    connect(m_ui->pushButton_WordlistFileBrowse,SIGNAL(clicked()),this,SLOT(buttonWordlistFileBrowseClicked()));
    connect(m_ui->pushButtonBrowsePathToJohn,SIGNAL(clicked()),this,SLOT(buttonBrowsePathToJohnClicked()));
    connect(m_ui->actionCopyToClipboard,SIGNAL(triggered()),this,SLOT(actionCopyToClipboardTriggered()));
    connect(m_ui->actionGuessPassword,SIGNAL(triggered()), this, SLOT(guessPassword()));

    connect(m_ui->tabSelectionToolBar, SIGNAL(actionTriggered(QAction*)), this, SLOT(tabsSelectionChanged(QAction*)));

    // Tableview and filtering-related signals
    m_ui->tableView_Hashes->setContextMenuPolicy(Qt::CustomContextMenu);
    m_hashTableContextMenu = new QMenu(this);
    m_hashTableContextMenu->addActions(QList<QAction*>() << m_ui->actionCopyToClipboard << m_ui->actionIncludeSelectedHashes
                                         << m_ui->actionExcludeSelectedHashes);
    connect(m_ui->tableView_Hashes, SIGNAL(customContextMenuRequested(const QPoint&)),
        this, SLOT(showHashesTableContextMenu(const QPoint&)));
    connect(m_ui->actionIncludeSelectedHashes, SIGNAL(triggered()), this, SLOT(includeSelectedHashes()));
    connect(m_ui->actionExcludeSelectedHashes, SIGNAL(triggered()), this, SLOT(excludeSelectedHashes()));
    connect(m_ui->checkBoxShowOnlyCheckedHashes, SIGNAL(toggled(bool)), m_hashTableProxy, SLOT(setShowCheckedRowsOnly(bool)));
    connect(m_ui->checkBoxShowOnlyCrackedHashes, SIGNAL(toggled(bool)), m_hashTableProxy, SLOT(setShowCrackedRowsOnly(bool)));
    // We create the app sessions data directory in $HOME if it does not exist
    if (!QDir().mkpath(JohnSession::sessionDir())) {
        QMessageBox::critical(this, tr("Johnny"),
            tr("Could not create sessions data director y(%1).\nCheck your permissions, disk space and restart Johnny.").arg(JohnSession::sessionDir()));
        qApp->quit();
    }

    // Load sessions
    m_settings.beginGroup("Sessions");
    QStringList sessionsList = m_settings.childGroups();
    for (int i = sessionsList.size()-1; i >= 0; i--) {
        QString sessionName = sessionsList[i];
        QString completePath = QDir(JohnSession::sessionDir()).filePath(sessionName);
        if (QFileInfo(completePath + ".rec").isReadable()) {
            QAction *fileAction = m_sessionMenu->addAction(sessionName);
            fileAction->setData(sessionName);
            m_sessionHistory.append(sessionName);
            QString fileNames = m_settings.value(sessionName + "/passwordFiles").toStringList().join(" ");
            fileAction->setToolTip(fileNames);
        } else {
            // The .rec may have been deleted manually by user, let's clean our settings
            m_settings.remove(sessionName);
        }
    }
    m_settings.endGroup();
#if QT_VERSION >= QT_VERSION_CHECK(5, 1, 0)
    m_sessionMenu->setToolTipsVisible(true);
#endif
    m_sessionMenu->addAction(m_ui->actionClearSessionHistory);

    // We fill form with default values. Then we load settings. When
    // there is no setting old value is used. So if there is no
    // configuration file then we get default values. Also it means
    // that when user pushes "reset to stored settings" button he
    // resets only fields with values. It is nice but may seem
    // strange: probably user would expect reset to be like default +
    // reset because storing of a part of settings is not normal
    // behaviour (only possible with upgrades).
    fillSettingsWithDefaults();

    // We load old settings.
    restoreSavedSettings();

    // Automatically open last session by default
    if (!m_sessionHistory.isEmpty()) {
        m_sessionCurrent = JohnSession(m_sessionHistory.first(), &m_settings);
        openLastSession();
    } else {
        restoreDefaultAttackOptions(false);
    }

    m_aboutWindow.setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint | Qt::WindowCloseButtonHint);
    Ui::aboutWidget aboutUi;
    aboutUi.setupUi(&m_aboutWindow);
    aboutUi.versionLabel->setText(tr("version ") + QCoreApplication::applicationVersion());
    connect(m_ui->actionAboutJohnny, SIGNAL(triggered()), &m_aboutWindow, SLOT(show()));
    connect(m_ui->actionCheckForUpdates, SIGNAL(triggered()), this, SLOT(checkForUpdates()));
    connect(aboutUi.checkUpdateButton, SIGNAL(clicked()), this, SLOT(checkForUpdates()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_terminate && (m_johnAttack.state() != QProcess::NotRunning)) {
        int answer = QMessageBox::question(
            this,
            tr("Johnny"),
            tr("An attack session is running, it will be terminated if you proceed. Do you really want to quit?"),
            QMessageBox::Yes | QMessageBox::No);
        if (answer == QMessageBox::No) {
            event->ignore();
            return;
        }
    }
    m_terminate = true;
    event->accept();
}

MainWindow::~MainWindow()
{
    m_johnAttack.stop();
    m_johnShow.stop();
    m_johnVersionCheck.stop();
    m_hashTypeChecker.stop();
    m_johnGuess.stop();

    delete m_ui;
    m_ui = 0;
    delete m_hashTable;
    m_hashTable = 0;
    delete m_johnShowTemp;
    m_johnShowTemp = 0;
}

void MainWindow::buttonWordlistFileBrowseClicked()
{
    // We pop a dialog to choose a file to open.
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter(tr("World list files (*.lst) ;; Dict files (*.dict);; Any files (*)"));

    if (dialog.exec()) {
        QString fileName = dialog.selectedFiles()[0];
        // We put file name into field for it.
        m_ui->lineEditWordlistFile->setText(fileName);
    }
}

void MainWindow::tabsSelectionChanged(QAction* action)
{
    int index = 0;
    if (action == m_ui->actionPasswordsTabClicked) {
        index = TAB_PASSWORDS;
    } else if (action == m_ui->actionOptionsTabClicked) {
        index = TAB_OPTIONS;
    } else if (action == m_ui->actionStatisticsTabClicked) {
        index = TAB_STATISTICS;
    } else if (action == m_ui->actionSettingsTabClicked) {
        index = TAB_SETTINGS;
    } else if (action ==  m_ui->actionConsoleLogTabClicked) {
        index = TAB_CONSOLE_LOG;
    }
    m_ui->actionCopyToClipboard->setEnabled(index == TAB_PASSWORDS);
    m_ui->contentStackedWidget->setCurrentIndex(index);
}

void MainWindow::replaceTableModel(PasswordFileModel *newTableModel)
{
    // Remove temporary file is exist
    if (m_johnShowTemp != NULL) {
        delete m_johnShowTemp;
        m_johnShowTemp = NULL;
    }

    // We delete existing model if any.
    if (m_hashTable != NULL) {
        delete m_hashTable;
        m_hashTable = NULL;
    }
    // We remember new model.
    m_hashTable = newTableModel;
    // We connect table view with new model.
    m_hashTableProxy->setSourceModel(newTableModel);
    // Hide formats column if not jumbo
    m_ui->tableView_Hashes->setColumnHidden(PasswordFileModel::FORMAT_COL, !m_isJumbo);
    connect(m_hashTable, SIGNAL(rowUncheckedByUser()), m_hashTableProxy, SLOT(checkBoxHasChanged()));
    // We build hash table for fast access.
    m_showTableMap = QMultiMap<QString, int>();
    // In case a newTableModel == NULL parameter is passed
    if(m_hashTable != NULL){
        for (int i = 0; i < m_hashTable->rowCount(); i++) {
            m_showTableMap.insert(
                m_hashTable->data(m_hashTable->index(i, 2)).toString(),
                i);
        }
    }
}

bool MainWindow::readPasswdFiles(const QStringList &fileNames)
{
    PasswordFileModel *model = new PasswordFileModel(this);
    if (model->readFiles(fileNames)) {
        resetFilters();
        // We replace existing model with new one.
        replaceTableModel(model);
        // After new model remembered we remember its file name.
        m_sessionPasswordFiles = fileNames;
        m_ui->passwordFilesLabel->setText(m_sessionPasswordFiles.join("; "));
        // We make a file with original hash in gecos to connect password
        // with original hash during `john --show`.
        if (!m_johnShowTemp) {
            m_johnShowTemp = new QTemporaryFile();
            if (m_johnShowTemp->open()) {
                QTextStream temp(m_johnShowTemp);
                for (int i = 0; i < m_hashTable->rowCount(); i++) {
                    QString user = m_hashTable->data(m_hashTable->index(i, 0)).toString();
                    QString hash = m_hashTable->data(m_hashTable->index(i, 2)).toString();
                    temp << user << ":" << hash << "::" << hash << '\n';
                }
                m_johnShowTemp->close();
            } else {
                QMessageBox::critical(
                    this,
                    tr("Johnny"),
                    tr("Can't open a temporary file. Your disk might be full."));
            }
        }
        callJohnShow(true);
        m_ui->widgetFilterOptions->setEnabled(true);
        m_ui->actionCopyToClipboard->setEnabled(m_ui->contentStackedWidget->currentIndex() == TAB_PASSWORDS);
        m_ui->actionStartAttack->setEnabled(true);
        m_ui->actionGuessPassword->setEnabled(true);
        m_ui->actionIncludeSelectedHashes->setEnabled(true);
        m_ui->actionExcludeSelectedHashes->setEnabled(true);
        if (m_isJumbo) {
            m_hashTypeChecker.setJohnProgram(m_pathToJohn);
            m_hashTypeChecker.setPasswordFiles(fileNames);
            m_hashTypeChecker.start();
        }
        // QSortFilterProxyModel isn't optimized for fast for dynamic filtering on big files
        if (model->rowCount() > DYNAMIC_FILTERING_HASH_LIMIT) {
#if QT_VERSION < QT_VERSION_CHECK(4, 7, 0)
            m_filterDirectivesLabel->show();
#else
            m_ui->lineEditFilter->setPlaceholderText(tr("Press Enter/Return key to apply filter"));
#endif
            disconnect(m_ui->lineEditFilter, SIGNAL(textEdited(QString)), this, SLOT(filterHashesTable()));
            connect(m_ui->lineEditFilter, SIGNAL(editingFinished()), this, SLOT(filterHashesTable()));
        } else {
#if QT_VERSION < QT_VERSION_CHECK(4, 7, 0)
            m_filterDirectivesLabel->hide();
#else
            m_ui->lineEditFilter->setPlaceholderText("");
#endif
            disconnect(m_ui->lineEditFilter, SIGNAL(editingFinished()), this, SLOT(filterHashesTable()));
            connect(m_ui->lineEditFilter, SIGNAL(textEdited(QString)), this, SLOT(filterHashesTable()));
        }
        return true;
    }
    QMessageBox::warning(
            this,
            tr("Johnny"),
            tr("Could not read desired password file(s)."));
    return false;
}

void MainWindow::openPasswordFile()
{
    // When user asks to open password file we should read desired
    // file, parse it and present values in the table. Model and view
    // simplifies presentation. We just make and fill model and then
    // we set it to existing view.

    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::ExistingFiles);
    if (dialog.exec()) {
        QStringList fileNames = dialog.selectedFiles();
        readPasswdFiles(fileNames);
        m_ui->actionResumeAttack->setEnabled(false);
    }
}

void MainWindow::openLastSession()
{
    m_sessionCurrent.load();
    QStringList passwordFiles= m_sessionCurrent.passwordFiles();

    if (readPasswdFiles(passwordFiles))
    {
        m_format = m_sessionCurrent.format();
        restoreSessionOptions();
        m_ui->actionResumeAttack->setEnabled(true);
    }
    else
    {
        QMessageBox::critical(this, "Johnny",
                              tr("Could not open the session password files. (%1)").arg(passwordFiles.join("; ")));
    }
}

void MainWindow::actionCopyToClipboardTriggered()
{
    if (!m_hashTable)
        return;
    QModelIndexList indexes = m_ui->tableView_Hashes->selectionModel()->selectedIndexes();
    if (indexes.count() == 0)
    {
        QMessageBox::warning(
            this,
            tr("Johnny"),
            tr("Nothing selected. Select rows/columns in the Passwords table to copy."));
        return;
    }

    QString out;
    if (indexes.count() == 1) {
        out = indexes.at(0).data().toString();
    } else {
        qSort(indexes);
        int previousRow = -1;
        // TODO: such table making works bad with ctrl+mouse
        //       selection. I'd say in such case not selected fields
        //       inside the rectangle should be exported as empty.
        foreach (const QModelIndex &index, indexes) {
            // TODO: do it faster.
            // TODO: check for tabs inside.
            // TODO: use mimetype to make good tables.
            if (previousRow == index.row()) {
                out += "\t";
            } else if (previousRow != -1) {
                out += "\n";
            }
            out += index.data().toString();
            previousRow = index.row();
        }
    }
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->clear();
    if (clipboard->supportsSelection())
        clipboard->setText(out, QClipboard::Selection);
    clipboard->setText(out);
}

bool MainWindow::checkSettings()
{
    if (m_pathToJohn.isEmpty()) {
        QMessageBox::critical(
            this,
            tr("Johnny"),
            tr("Please specify the path to JohntheRipper in settings."));
        return false;
    }
    return true;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    if (!event || !watched || !watched->isWidgetType())
        return false;

    if(event->type() == QEvent::StyleAnimationUpdate)
    {
        QWidget *widget = (QWidget*) watched;
        if (widget->inherits("QProgressBar"))
            return true;
    }
#endif
    return false;
}

void MainWindow::startAttack()
{
    if (!checkSettings())
        return;

    // Session for johnny
    QString date = QDateTime::currentDateTime().toString("MM-dd-yy-hh-mm-ss");
    m_sessionCurrent = JohnSession(date, &m_settings);
    QString sessionFile = m_sessionCurrent.filePath() + ".rec";

    if (QFileInfo(sessionFile).isReadable())
    {
        QMessageBox::StandardButton button =
                QMessageBox::question(this,
                tr("Johnny"),
                tr("A session file already exists with this name (%1). Do you want to overwrite?").arg(sessionFile),
                QMessageBox::Yes | QMessageBox::No);
        if (button == QMessageBox::Yes)
        {
            // Remove existing session .rec file to avoid issues with JtR
            if(!QFile(sessionFile).remove())
            {
                QMessageBox::warning(this, tr("Warning"),
                                     tr("Unable to remove file session file %1").arg(sessionFile));
            }
        }
    }

    QStringList parameters = saveAttackParameters();
    m_sessionCurrent.save();
    parameters << QString("--session=%1").arg(m_sessionCurrent.filePath());

    // We check that we have file name.
    if (!m_sessionPasswordFiles.isEmpty()) {
        QList<int> unselectedRows = m_sessionCurrent.unselectedRows();
        // If some hashes are unselected, write a new file with only selected hashes
        if (unselectedRows.size() > 0) {
            QString newFilePath = m_sessionCurrent.filePath() + ".pw";
            int currentRow = 0;
            for (int fileCount = 0; fileCount < m_sessionPasswordFiles.size(); fileCount++) {
                QFile file(m_sessionPasswordFiles[fileCount]);
                QFile newFile(newFilePath);
                if (file.open(QIODevice::ReadOnly | QIODevice::Text)
                        && newFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                    QTextStream out(&newFile);
                    while (!file.atEnd()) {
                        QString line = file.readLine();
                        if (unselectedRows.isEmpty() || unselectedRows.first() != currentRow) {
                            out << line;
                        } else if(!unselectedRows.isEmpty()) {
                            unselectedRows.removeFirst();
                        }
                        currentRow++;
                    }
                    parameters << newFilePath;
                }
            }
            // Else, pass the file AS IS to john
        } else {
            parameters << m_sessionPasswordFiles;
        }
        startJohn(parameters);
    } else {
        QMessageBox::warning(
            this,
            tr("Johnny"),
            tr("No password files specified."));
    }
}

/* This function doesn't return the parameters of the current john process,
 * it returns the selected parameters in Johnny (UI-side) */
QStringList MainWindow::saveAttackParameters()
{
    m_ui->sessionNameLabel->setText(m_sessionCurrent.name());
    m_sessionCurrent.setPasswordFiles(m_sessionPasswordFiles);

    QStringList parameters;
    // We prepare parameters list from options section.
    // General options
    // Format
    if (m_ui->formatComboBox->currentText() != tr("Auto detect")) {
        // We have one list for formats and subformats. Subformats
        // contain short description after it.
        // Strings could be like:
        // des
        // md5_gen(0): md5($p)  (raw-md5)
        // md5_gen(1001) md5(md5(md5(md5($p))))
        // User formats does not have semicolon between format name
        // and description. So we will restrict our pattern with right
        // brace.
        // NOTE: $ john -format:md5-gen -subformat:md5_gen(0)
        //       is deprecated. So we will use
        //       $ john -format:md5_gen(0)
        //       instead. See
        //       http://www.openwall.com/lists/john-users/2011/08/17/2
        // We remember format key to be used with '-show' to take
        // progress.
        m_format = "--format=" + m_ui->formatComboBox->currentText();
        // Now we have '--format=format' or '--format=format(N)description'.
        // So we truncate string to ')' if brace is in string.
        //
        // We try to find ')'.
        int index = m_format.indexOf(")");
        // We check that we have brace in string.
        if (index >= 0) {
            // We truncate line to index keeping index.
            m_format.truncate(index + 1);
        }
        // We add format key onto parameters list.
        parameters << m_format;
    } else {
        // In case we do not have explicit format we erase remembered
        // key.
        m_format.clear();
    }
    m_sessionCurrent.setFormat(m_format);
    m_sessionCurrent.setFormatUI(m_ui->formatComboBox->currentText());

    // Modes
    QWidget* selectedMode = m_ui->attackModeTabWidget->currentWidget();
    if (selectedMode == m_ui->defaultModeTab) {
        // Default behaviour - no modes
        // There are no options here.
        m_sessionCurrent.setMode(JohnSession::DEFAULT_MODE);
    } else if (selectedMode == m_ui->singleModeTab) {
        // "Single crack" mode
        parameters << "--single";
        m_sessionCurrent.setMode(JohnSession::SINGLECRACK_MODE);
        // External mode, filter
        if (m_ui->checkBox_SingleCrackModeExternalName->isChecked()) {
            parameters << ("--external=" + m_ui->lineEditSingleCrackModeExternalName->text());
            m_sessionCurrent.setExternalName(m_ui->lineEditSingleCrackModeExternalName->text());
        }

    } else if (selectedMode == m_ui->wordlistModeTab) {
        // Wordlist mode
        m_sessionCurrent.setMode(JohnSession::WORDLIST_MODE);
        parameters << ("--wordlist=" + m_ui->lineEditWordlistFile->text());
        m_sessionCurrent.setWordlistFile(m_ui->lineEditWordlistFile->text());

        // Rules
        if (m_ui->checkBox_WordlistModeRules->isChecked()) {
            m_sessionCurrent.setRules(m_ui->lineEdit_WordlistRules->text());
            if (m_ui->lineEdit_WordlistRules->text().isEmpty()) {
                parameters << "--rules";
            } else {
                parameters << ("--rules=" + m_ui->lineEdit_WordlistRules->text());
            }
        }
        // External mode, filter
        if (m_ui->checkBox_WordlistModeExternalName->isChecked()) {
            parameters << ("--external=" + m_ui->lineEditWordlistModeExternalName->text());
            m_sessionCurrent.setExternalName(m_ui->lineEditWordlistModeExternalName->text());
        }
    } else if (selectedMode == m_ui->incrementalModeTab) {
        // "Incremental" mode
        // It could be with or without name.
        m_sessionCurrent.setMode(JohnSession::INCREMENTAL_MODE);
        if (m_ui->checkBox_IncrementalModeName->isChecked()) {
            // With name
            parameters << ("--incremental=" + m_ui->lineEditIncrementalModeName->text());
            m_sessionCurrent.setCharset(m_ui->lineEditIncrementalModeName->text());
        } else {
            // Without name
            parameters << "--incremental";
        }
        // External mode, filter
        if (m_ui->checkBox_IncrementalModeExternalName->isChecked()) {
            parameters << ("--external=" + m_ui->lineEditIncrementalModeExternalName->text());
            m_sessionCurrent.setExternalName(m_ui->lineEditIncrementalModeExternalName->text());
        }
    } else if (selectedMode == m_ui->externalModeTab) {
        // External mode
        m_sessionCurrent.setMode(JohnSession::EXTERNAL_MODE);
        parameters << ("--external=" + m_ui->lineEditExternalModeName->text());
        m_sessionCurrent.setExternalName(m_ui->lineEditExternalModeName->text());
    }

    // Selectors
    if (m_ui->checkBox_LimitUsers->isChecked()) {
        parameters << ("--users=" + m_ui->lineEditLimitUsers->text());
        m_sessionCurrent.setLimitUsers(m_ui->lineEditLimitUsers->text());
    }
    if (m_ui->checkBox_LimitGroups->isChecked()) {
        parameters << ("--groups=" + m_ui->lineEditLimitGroups->text());
        m_sessionCurrent.setLimitGroups(m_ui->lineEditLimitGroups->text());
    }
    if (m_ui->checkBox_LimitShells->isChecked()) {
        parameters << ("--shells=" + m_ui->lineEditLimitShells->text());
        m_sessionCurrent.setLimitShells(m_ui->lineEditLimitShells->text());
    }
    if (m_ui->checkBox_LimitSalts->isChecked()) {
        parameters << (QString("--salts=%1").arg(m_ui->spinBox_LimitSalts->value()));
        m_sessionCurrent.setLimitSalts(m_ui->spinBox_LimitSalts->value());
    }

    // Advanced options
    if (m_ui->checkBox_UseFork->isChecked()) {
        parameters << (QString("--fork=%1").arg(m_ui->spinBox_nbOfProcess->value()));
        m_sessionCurrent.setNbProcess(m_ui->spinBox_nbOfProcess->value());
    }
    m_sessionCurrent.setNbOpenMPThreads(m_ui->spinBox_nbOfOpenMPThread->value());
    if (m_ui->checkBox_EnvironmentVar->isChecked()) {
       m_sessionCurrent.setEnvironmentVariables(m_ui->lineEdit_EnvironmentVar->text());
    }
    // Save unselected rows
    QList<int> unselectedRows;
    for (int i = 0; i < m_hashTable->rowCount(); i++) {
        if (m_hashTable->data(m_hashTable->index(i,0),Qt::CheckStateRole) == Qt::Unchecked) {
            unselectedRows.append(i);
        }
    }
    m_sessionCurrent.setUnselectedRows(unselectedRows);

    return parameters;
}

void MainWindow::startJohn(QStringList args)
{
    // To start John we have predefined process object. That object's
    // signals are already connected with our slots. So we need only
    // start it.

    appendLog(QTime::currentTime().toString("[hh:mm:ss] ") + m_pathToJohn + " " + args.join(" ") + '\n');

    //We set up environment variables, ex : useful for openMP
    QProcessEnvironment env;
    // If default is chosen, we don't specify OMP_NUM_THREADS and john will choose the number of
    // threads based on the number of processors.
    if (m_ui->spinBox_nbOfOpenMPThread->text() != m_ui->spinBox_nbOfOpenMPThread->specialValueText()) {
        env.insert("OMP_NUM_THREADS", m_ui->spinBox_nbOfOpenMPThread->text()); // Add an environment variable
    }

    // User specified environment variables
    if (m_ui->checkBox_EnvironmentVar->isChecked()) {
        // Parse the input
        QStringList varList = m_ui->lineEdit_EnvironmentVar->text().split(",", QString::SkipEmptyParts);
        for (int i = 0; i < varList.size(); i++) {
            QStringList varPair = varList[i].split("=", QString::SkipEmptyParts);
            if (varPair.size() == 2) { // we assume value of variable doesn't have = inside
                env.insert(varPair[0].trimmed(), varPair[1].trimmed());
            } else {
                QMessageBox::warning(
                        this,
                        tr("Environment variables"),
                        tr("The format to set environment variable must be in the format : varName1="
                           "value, varName2=value etc.. "));
            }
        }
    }
    m_johnAttack.setEnv(env);
    m_johnAttack.setArgs(args);
    m_johnAttack.setJohnProgram(m_pathToJohn);
    m_johnAttack.start();
}


void MainWindow::resumeAttack()
{
    if (!checkSettings())
        return;

    QStringList parameters;
    parameters << QString("--restore=%1").arg(m_sessionCurrent.filePath());

    startJohn(parameters);
}

void MainWindow::updateJohnOutput()
{
    //read output and error buffers
    appendLog(m_johnAttack.readAllStandardOutput()
              + m_johnAttack.readAllStandardError());

    // NOTE: Probably here we want to parse John's output, catch newly
    //       cracked passwords and so on. However John's output is buffered.
    //       So we do not obtain it as soon as it occurs. Timer and
    //       --show work for us.
}

void MainWindow::pauseAttack()
{
    m_johnAttack.stop();
}

void MainWindow::showJohnStarted()
{
    // We disable all buttons.
    m_ui->actionPauseAttack->setEnabled(false);
    m_ui->actionStartAttack->setEnabled(false);
    m_ui->actionResumeAttack->setEnabled(false);
    m_ui->actionOpenPassword->setEnabled(false);
    m_ui->actionOpenSession->setEnabled(false);

    // When John starts we enable stop button.
    m_ui->actionPauseAttack->setEnabled(true);
    // When John starts we start capturing passwords.
    // TODO: When user change respective setting time for current run
    //       is not changed. Probably user expects other.
    // TODO: Should we distinguish settings for current run and for
    //       next runs? At least we should mark settings that could be
    //       changed while John works as having such behaviour.
    // NOTE: m_timeIntervalPickCracked is limited to int (maximum is
    //       MAX_INT) but here we multiple it by 1000. So we could
    //       have integer overflow. At this time value is borrowed from
    //       spin box. It could have customized limit. But currently
    //       we do not want to set it up at runtime so we should use
    //       value appropriate for multiple platforms but enough big
    //       to be comfortable for users.
    //       So current value corresponds to one day. However it is
    //       not suitable for platforms with int of two bytes.
    m_showTimer.start(m_timeIntervalPickCracked * 1000);
    // If we continue cracking than there could already be cracked
    // passwords so we check status.
    callJohnShow();
}

void MainWindow::showJohnError(QProcess::ProcessError error)
{
    QString message;
    switch (error) {
    case QProcess::FailedToStart:
        message = tr("John failed to start."
                     "Check your path to John."
                     "Check permissions on the executable.");
        break;

    case QProcess::Crashed:
        message = tr("John crashed. Verify the Console Log for details.");
        break;

    case QProcess::Timedout:
        message = tr("A timed out error occurred to John.");
        break;

    case QProcess::WriteError:
        message = tr("A write error occurred to John.");
        break;

    case QProcess::ReadError:
        message = tr("A read error occurred to John.");
        break;

    case QProcess::UnknownError:
        message = tr("An unknown problem occurred to John. Verify Console Log for any details.");
        break;
    }

    QMessageBox::critical(this, tr("Johnny"), message + " (john is " + m_pathToJohn + ")");

    if (QObject::sender() == &m_johnGuess)
    {
        m_ui->actionGuessPassword->setEnabled(true);
    }
}

void MainWindow::showJohnFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode);
    appendLog(CONSOLE_LOG_SEPARATOR);
    QString sessionName(m_sessionCurrent.name());

    bool isNewSession = !m_sessionHistory.contains(sessionName);
    bool isRecReadable = QFileInfo(m_sessionCurrent.filePath() + ".rec").isReadable();
    if ((isNewSession == true) && (isRecReadable == true)) {
        // New session saved by john, add it to the list
        QAction* action = new QAction(sessionName, this);
        m_sessionMenu->insertAction(m_sessionMenu->actions()[0], action);
        action->setData(sessionName);
        m_sessionHistory.append(sessionName);
        action->setToolTip(m_sessionPasswordFiles.join(" "));
    } else if ((isNewSession == false) && (isRecReadable == false)) {
        // An old session (which was resumed) terminated and it can no longer be resumed (john deleted .rec)
        // so we remove it from the session history list to have an error-prone UI
        m_sessionHistory.removeOne(sessionName);
        m_sessionCurrent.remove();
        foreach(QAction* actions, m_sessionMenu->actions()) {
            if (actions->data().toString() == sessionName) {
                    m_sessionMenu->removeAction(actions);
            }
        }
    }

    // When John finishes we enable start button and disable stop
    // button.
    m_ui->actionPauseAttack->setEnabled(false);
    m_ui->actionStartAttack->setEnabled(true);
    m_ui->actionOpenPassword->setEnabled(true);
    m_ui->actionOpenSession->setEnabled(true);
    m_ui->actionResumeAttack->setEnabled(m_sessionHistory.contains(sessionName) && isRecReadable);

    if (exitStatus == QProcess::CrashExit) {
        qDebug() << "JtR seems to have crashed.";
        return;
    }
    // When John stops we need to stop timer and to look status last
    // time.
    m_showTimer.stop();
    callJohnShow();
}

void MainWindow::callJohnShow(bool showAllFormats)
{
    // Give a chance to terminate cleanly
    if (m_johnShow.state() != QProcess::NotRunning)
        m_johnShow.stop();

    QStringList args;
    // We add current format key if it is not empty.
    if (!m_format.isEmpty() && !showAllFormats)
        args << m_format;
    args << "--show" << m_johnShowTemp->fileName();
    m_johnShow.setJohnProgram(m_pathToJohn);
    m_johnShow.setArgs(args);
    m_johnShow.start();
}

void MainWindow::readJohnShow()
{
    // We read all output.
    QString formattedFormat(m_format);
    formattedFormat.remove("--");
    QByteArray output = m_johnShow.readAllStandardOutput();
    QTextStream outputStream(output);
    // We parse it.
    // We read output line by line and take user name and password.
    // Then we find a row with such user and insert password there.
    QString line;
    line = outputStream.readLine();
    // If john did not yet cracked anything then john does not emit
    // empty line before statistics.
    QString firstLine;
    firstLine = line;
    // We read to the end or before empty line.
    bool hasTableChanged = false;
    while (!line.isNull() && line != "") {
        line.remove(QRegExp("\\r?\\n"));
        // We split lines to fields.
        int left = line.indexOf(":");
        int right = line.lastIndexOf("::");
        QString password = line.mid(left + 1, right - left - 1);
        QString hash = line.mid(right + 2);
        // We handle password.
        // If we found user then we put password in table.
        foreach (int row, m_showTableMap.values(hash)) {
            hasTableChanged = true;
            m_hashTable->setData(
                m_hashTable->index(row, PasswordFileModel::PASSWORD_COL),
                password);
        }
        // We remove value to speed up.
        m_showTableMap.remove(hash);
        // We continue reading with next line.
        line = outputStream.readLine();
    }
    if (hasTableChanged)
        m_hashTableProxy->crackingHasChanged();

    QString lastLine;
    if (!line.isNull()) {
        // We are on the last line.
        // We take counts of cracked passwords and of left hashes.
        // We read count of cracked password hashes.
        lastLine = outputStream.readLine();
    } else {
        lastLine = firstLine;
    }
    QRegExp crackedLeft("(\\d+)\\D+(\\d+)");
    int pos = crackedLeft.indexIn(lastLine);
    if (pos > -1) {
        int crackedCount = crackedLeft.cap(1).toInt();
        int leftCount = crackedLeft.cap(2).toInt();
        // Update progress bar
        if (crackedCount + leftCount == 0) {
            // There are no hashes.
            m_ui->progressBar->setRange(0, 1);
            m_ui->progressBar->setValue(0);
            m_ui->progressBar->setFormat(
                tr("No hashes loaded [%1], see Console log").arg(
                    formattedFormat));
        } else {
            m_ui->progressBar->setRange(0, crackedCount + leftCount);
            m_ui->progressBar->setValue(crackedCount);
            m_ui->progressBar->setFormat(
                tr("%p% (%v/%m: %1 cracked, %2 left) [%3]").arg(crackedCount)
                                                .arg(leftCount).arg(formattedFormat));
        }
#ifdef Q_OS_OSX
            if(m_progressStatsLabel)
                m_progressStatsLabel->setText(m_ui->progressBar->text());
#endif
    } else {
        // TODO: Error: unexpected john output.
        // TODO: Unknown cyphertext format is here. Read stderr to check exactly.
    }
}

// Settings page code
// NOTE: To add new setting you should add data member to form's
//       class, then you should add copying line to every method on
//       the list:
//       fillSettingsWithDefaults,
//       applySettings,
//       applyAndSaveSettings,
//       restoreLastSavedSettings,
//       And of course you should put elements on the form.
//       And you must connect the signal eventChanged of the widget to the slot
//       applyAndSaveSettings() to take care of auto-saving

void MainWindow::warnAboutDefaultPathToJohn()
{
    QMessageBox::warning(
        this,
        tr("Johnny: default path to john"),
        tr("Currently Johnny filled settings with default path to John the Ripper (%1). "
           "You could set your preferred path in settings "
           "(just use 'john' there to make Johnny search for John the Ripper "
           "in PATH on every invocation of John the Ripper). "
           "If you are satisfied with defaults then save settings to avoid this message.").arg(
               m_ui->lineEditPathToJohn->text()));
}

void MainWindow::fillSettingsWithDefaults()
{
    QStringList possiblePaths;
    QString john;
    // Find john on system path, which is determined by PATH variable
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    foreach (QString dir, env.value("PATH").split(":")) {
        possiblePaths << QDir(dir).filePath("john");
    }
    possiblePaths << QDir::current().filePath("john"); // in the same directory than johnny
#if defined Q_OS_WIN
    foreach(QString dir, possiblePaths) {
        possiblePaths.append(".exe");
    }
#endif

#else
    QString johnSystemPath = QStandardPaths::findExecutable("john", QStringList());
    if(!johnSystemPath.isEmpty())
        possiblePaths << johnSystemPath;

    // Predefined defaults
    // John might be in in the same directory than johnny
    QString johnOtherPaths = QStandardPaths::findExecutable("john", QStringList(QDir::currentPath()));
    if(!johnOtherPaths.isEmpty())
        possiblePaths << johnOtherPaths;
#endif

    // Find first readable, executable file from possible
    foreach (QString path, possiblePaths) {
        QFileInfo iJohn(path);
        if (iJohn.isReadable() && iJohn.isExecutable()) {
            john = path;
            break;
        }
    }
    m_ui->lineEditPathToJohn->blockSignals(true);
    m_ui->spinBoxTimeIntervalPickCracked->blockSignals(true);
    m_ui->lineEditPathToJohn->setText(john);
    m_ui->spinBoxTimeIntervalPickCracked->setValue(INTERVAL_PICK_CRACKED);
    m_ui->lineEditPathToJohn->blockSignals(false);
    m_ui->spinBoxTimeIntervalPickCracked->blockSignals(false);
}

void MainWindow::buttonBrowsePathToJohnClicked()
{
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::ExistingFile);
    if (dialog.exec()) {
        QString fileName = dialog.selectedFiles()[0];
        // We put file name into field for it.
        m_ui->lineEditPathToJohn->setText(fileName);
        applyAndSaveSettings();
    }
}

void MainWindow::applySettings()
{
    // We verify john version
    QString newJohnPath = m_ui->lineEditPathToJohn->text();
    if ((m_pathToJohn != newJohnPath) && !newJohnPath.isEmpty()) {
        m_johnVersionCheck.setJohnProgram(newJohnPath);
        m_johnVersionCheck.start();
    }
    // We copy settings from elements on the form to the settings
    // object with current settings.
    m_pathToJohn = newJohnPath;
    m_timeIntervalPickCracked = m_ui->spinBoxTimeIntervalPickCracked->value();

    // If the language changed, retranslate the UI
    Translator &translator = Translator::getInstance();
    QString newLanguage = m_ui->comboBoxLanguageSelection->currentText().toLower();
    if (newLanguage != translator.getCurrentLanguage().toLower()) {
        translator.translateApplication(qApp,newLanguage);
        m_ui->retranslateUi(this);
    }
}

void MainWindow::applyAndSaveSettings()
{
    applySettings();
    m_settings.setValue("PathToJohn", m_ui->lineEditPathToJohn->text());
    m_settings.setValue("TimeIntervalPickCracked", m_ui->spinBoxTimeIntervalPickCracked->value());
    m_settings.setValue("Language", m_ui->comboBoxLanguageSelection->currentText().toLower());
}

void MainWindow::restoreSavedSettings()
{
    // We copy stored settings to the form and then invoke applySettings()
    m_ui->lineEditPathToJohn->blockSignals(true);
    m_ui->spinBoxTimeIntervalPickCracked->blockSignals(true);
    m_ui->comboBoxLanguageSelection->blockSignals(true);
    QString settingsPathToJohn = m_settings.value("PathToJohn").toString();
    m_ui->lineEditPathToJohn->setText(
        settingsPathToJohn == ""
        ? m_ui->lineEditPathToJohn->text()
        : settingsPathToJohn);
    m_ui->spinBoxTimeIntervalPickCracked->setValue(
        m_settings.value("TimeIntervalPickCracked").toString() == ""
        ? m_ui->spinBoxTimeIntervalPickCracked->value()
        : m_settings.value("TimeIntervalPickCracked").toInt());
    int languageIndex = m_ui->comboBoxLanguageSelection->findText(m_settings.value("Language").toString());
    if (languageIndex != -1) {
        m_ui->comboBoxLanguageSelection->setCurrentIndex(languageIndex);
    }
    m_ui->lineEditPathToJohn->blockSignals(false);
    m_ui->spinBoxTimeIntervalPickCracked->blockSignals(false);
    m_ui->comboBoxLanguageSelection->blockSignals(false);
    applySettings();

}

void MainWindow::updateStatistics()
{
    if (m_johnAttack.state() == QProcess::Running) {
        qint64 secondsElapsed = m_johnAttack.startTime().secsTo(QDateTime::currentDateTime());
        qint64 days    = secondsElapsed / 86400;
        qint64 hours   = (secondsElapsed % 86400) / 3600;
        qint64 minutes = ((secondsElapsed % 86400) % 3600) / 60;
        qint64 seconds = ((secondsElapsed % 86400) % 3600) % 60;

        QString workingTime;
        QTextStream stream(&workingTime);
        stream << days << tr(":");
        // Hours, minutes and seconds have padding with zeroes to two
        // chars.
        stream.setPadChar('0');
        stream.setFieldAlignment(QTextStream::AlignRight);
        stream << qSetFieldWidth(2) << hours << qSetFieldWidth(1) << tr(":");
        stream << qSetFieldWidth(2) << minutes << qSetFieldWidth(1) << tr(":");
        stream << qSetFieldWidth(2) << seconds;

        m_ui->labelStatisticsWorkingTime->setText(workingTime);
    } else {
        m_ui->labelStatisticsWorkingTime->setText("-");
    }
    callJohnShow();
}

/*
 * Since QPlainTextEdit::appendPlainText() add newLines without asking us and
 * QPlainTextEdit::insertPlainText() insert text by default at the cursor pos,
 * which can be modified by the user, this function assures the text is
 * inserted at the end without new line by default.
*/

void MainWindow::appendLog(const QString& text)
{
    // Preserving cursor preserves selection by user
    QTextCursor prev_cursor = m_ui->consoleLogTextEdit->textCursor();
    m_ui->consoleLogTextEdit->moveCursor(QTextCursor::End);
    m_ui->consoleLogTextEdit->insertPlainText(text);
    m_ui->consoleLogTextEdit->setTextCursor(prev_cursor);
}

/* This slot is triggered when the types changed. This is probably because :
 * 1) a new password file has been loaded OR 2) old file with a new jumbo john was used
 */
void MainWindow::updateHashTypes(const QStringList &pathToPwdFile, const QStringList &listOfTypesInFile,
                                 const QStringList &detailedTypesPerRow)
{
    PasswordFileModel* model = m_hashTable;
    if ((model != NULL) && (pathToPwdFile == m_sessionPasswordFiles)) {
        // We know that the right file is still opened so the signal
        // isn't too late, otherwise we don't replace the model
        model->fillHashTypes(detailedTypesPerRow);
        QString savedFormat = m_ui->formatComboBox->currentText();
        // For jumbo, we list only available formats in file in attack option
        m_ui->formatComboBox->clear();
        m_ui->formatComboBox->addItem(tr("Auto detect"));
        m_ui->formatComboBox->addItems(listOfTypesInFile);
        // Restore user's selection
        int indexSavedFormat = m_ui->formatComboBox->findText(savedFormat);
        if (indexSavedFormat != -1) {
            m_ui->formatComboBox->setCurrentIndex(indexSavedFormat);
        } else if(savedFormat.isEmpty()){
            m_ui->formatComboBox->setEditText(savedFormat);
        }
    }
}

// Enable/Disable all features that are jumbo related in this method
void MainWindow::setAvailabilityOfFeatures(bool isJumbo)
{
    bool wasLastVersionJumbo = m_isJumbo;
    m_isJumbo = isJumbo;
    if ((wasLastVersionJumbo == false) && (isJumbo == true) && (!m_sessionPasswordFiles.isEmpty())) {
        m_hashTypeChecker.setJohnProgram(m_pathToJohn);
        m_hashTypeChecker.setPasswordFiles(m_sessionPasswordFiles);
        m_hashTypeChecker.start();
    }
    m_ui->tableView_Hashes->setColumnHidden(PasswordFileModel::FORMAT_COL, !isJumbo);
    m_ui->actionFilterFormatColumn->setEnabled(isJumbo);
    if (!isJumbo) {
        // Add default format list supported by core john
        QStringList defaultFormats;
        defaultFormats << tr("Auto detect") << "descrypt" << "bsdicrypt" << "md5crypt"
                       << "bcrypt" << "AFS" << "LM" << "crypt" << "tripcode" << "dummy";
        QString savedFormat = m_ui->formatComboBox->currentText();
        m_ui->formatComboBox->clear();
        m_ui->formatComboBox->addItems(defaultFormats);
        // Restore user's selection
        int indexSavedFormat = m_ui->formatComboBox->findText(savedFormat);
        if (indexSavedFormat != -1) {
            m_ui->formatComboBox->setCurrentIndex(indexSavedFormat);
        } else if (!savedFormat.isEmpty()){
            m_ui->formatComboBox->setEditText(savedFormat);
        }
    }
}

void MainWindow::verifyJohnVersion()
{
    QString output = m_johnVersionCheck.readAllStandardOutput();
    bool isJumbo = output.contains("jumbo", Qt::CaseInsensitive);
    setAvailabilityOfFeatures(isJumbo);
    bool isForkEnabled = output.contains("fork", Qt::CaseInsensitive);
    m_ui->widgetFork->setVisible(isForkEnabled);
}

void MainWindow::actionOpenSessionTriggered(QAction* action)
{
    if ((action == m_ui->actionClearSessionHistory) && !m_sessionHistory.isEmpty()) {
        QDir dir(JohnSession::sessionDir());
        dir.setNameFilters(QStringList() << "*.log" << "*.johnny" << "*.rec" << "*.pw");
        dir.setFilter(QDir::Files);
        foreach (QString dirFile, dir.entryList()) {
            dir.remove(dirFile);
        }
        foreach(QAction* actions, m_sessionMenu->actions()) {
            if (m_sessionHistory.removeOne(actions->data().toString())) {
                    m_sessionMenu->removeAction(actions);
            }
        }
        m_sessionCurrent = JohnSession("", &m_settings);
        m_settings.remove("Sessions");
        m_ui->actionResumeAttack->setEnabled(false);
    } else {
        QString fileName = action->data().toString();
        if (!fileName.isEmpty()) {
            m_sessionCurrent = JohnSession(fileName, &m_settings);
            openLastSession();
        }
    }
}

void MainWindow::guessPassword()
{
    bool proceed;
    QString pwGuess = QInputDialog::getText(this, tr("Password Guessing"),
                                            tr("Your passphrase guess:"),
                                            QLineEdit::Normal, "", &proceed);
    if (proceed && !pwGuess.isEmpty())
    {
        m_ui->actionGuessPassword->setEnabled(false);
        m_johnGuess.setJohnProgram(m_pathToJohn);
        QStringList args;
        args << "--stdin";
        if(!m_format.isEmpty())
            args << m_format;
        args << m_sessionPasswordFiles;
        m_johnGuess.setArgs(args);
        m_johnGuess.start();
        m_johnGuess.write(pwGuess);
        m_johnGuess.closeWriteChannel();
    }
}

void MainWindow::guessPasswordFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode);
    m_ui->actionGuessPassword->setEnabled(true);
    if (exitStatus == QProcess::CrashExit) {
        qDebug() << "JtR seems to have crashed.";
        return;
    }
    callJohnShow(true);
}

void MainWindow::restoreSessionOptions()
{
    restoreDefaultAttackOptions();
    // Start restoring required UI fields
    m_ui->sessionNameLabel->setText(m_sessionCurrent.name());
    m_format = m_sessionCurrent.format();
    m_ui->formatComboBox->setEditText(m_sessionCurrent.formatUI());
    JohnSession::AttackMode mode = m_sessionCurrent.mode();
    if (mode == JohnSession::SINGLECRACK_MODE) {
        m_ui->attackModeTabWidget->setCurrentWidget(m_ui->singleModeTab);
        // External mode, filter
        if(!m_sessionCurrent.externalName().isNull()) {
            m_ui->checkBox_SingleCrackModeExternalName->setChecked(true);
            m_ui->lineEditSingleCrackModeExternalName->setText(m_sessionCurrent.externalName());
        }
    } else if (mode == JohnSession::WORDLIST_MODE) {
        m_ui->attackModeTabWidget->setCurrentWidget(m_ui->wordlistModeTab);
        m_ui->lineEditWordlistFile->setText(m_sessionCurrent.wordlistFile());
        //Rules
        if (!m_sessionCurrent.rules().isNull()) {
            m_ui->checkBox_WordlistModeRules->setChecked(true);
            m_ui->lineEdit_WordlistRules->setText(m_sessionCurrent.rules());
        }
        // External mode, filter
        if (!m_sessionCurrent.externalName().isNull()) {
            m_ui->checkBox_WordlistModeExternalName->setChecked(true);
            m_ui->lineEditWordlistModeExternalName->setText(m_sessionCurrent.externalName());
        }
    } else if (mode == JohnSession::INCREMENTAL_MODE) {
        m_ui->attackModeTabWidget->setCurrentWidget(m_ui->incrementalModeTab);
        // "Incremental" mode
        // It could be with or without name.
        if (!m_sessionCurrent.charset().isNull()) {
            m_ui->checkBox_IncrementalModeName->setChecked(true);
            m_ui->lineEditIncrementalModeName->setText(m_sessionCurrent.charset());
        }
        // External mode, filter
        if (!m_sessionCurrent.externalName().isNull()) {
            m_ui->checkBox_IncrementalModeExternalName->setChecked(true);
            m_ui->lineEditIncrementalModeExternalName->setText(m_sessionCurrent.externalName());
        }
    } else if (mode == JohnSession::EXTERNAL_MODE) {
        m_ui->attackModeTabWidget->setCurrentWidget(m_ui->externalModeTab)  ;
        m_ui->lineEditExternalModeName->setText(m_sessionCurrent.externalName());
    } else {
        m_ui->attackModeTabWidget->setCurrentWidget(m_ui->defaultModeTab);
    }

    // Selectors
    if (!m_sessionCurrent.limitUsers().isNull()) {
        m_ui->checkBox_LimitUsers->setChecked(true);
        m_ui->lineEditLimitUsers->setText(m_sessionCurrent.limitUsers());
    }
    if (!m_sessionCurrent.limitGroups().isNull()) {
        m_ui->checkBox_LimitGroups->setChecked(true);
        m_ui->lineEditLimitGroups->setText(m_sessionCurrent.limitGroups());
    }
    if (!m_sessionCurrent.limitShells().isNull()) {
        m_ui->checkBox_LimitShells->setChecked(true);
        m_ui->lineEditLimitShells->setText(m_sessionCurrent.limitShells());
    }
    if (m_sessionCurrent.limitSalts() >= 0) {
        m_ui->checkBox_LimitSalts->setChecked(true);
        m_ui->spinBox_LimitSalts->setValue(m_sessionCurrent.limitSalts());
    }

    // Advanced options
    if (m_sessionCurrent.isForkEnabled()) {
        m_ui->checkBox_UseFork->setChecked(true);
        int nbOfProcess = m_sessionCurrent.nbProcess();
        // In case the restored session ideal thread count is greather than current maximum (ex: user changed VM settings),
        // we have to restore the right previous session value.
        if (nbOfProcess > m_ui->spinBox_nbOfProcess->maximum()) {
            m_ui->spinBox_nbOfProcess->setMaximum(nbOfProcess);
        }
        m_ui->spinBox_nbOfProcess->setValue(nbOfProcess);
    }
    m_ui->spinBox_nbOfOpenMPThread->setValue(m_sessionCurrent.nbOpenMPThreads());

    if (!m_sessionCurrent.environmentVariables().isNull()) {
        m_ui->checkBox_EnvironmentVar->setChecked(true);
        m_ui->lineEdit_EnvironmentVar->setText(m_sessionCurrent.environmentVariables());
    }
    // Unselected hashes
    QList<int> unselectedRows = m_sessionCurrent.unselectedRows();
    for (int i = 0; i < unselectedRows.count(); i++) {
        m_hashTable->setData(m_hashTable->index(unselectedRows[i],0),UNCHECKED_PROGRAMMATICALLY,Qt::CheckStateRole);
    }
    if (unselectedRows.count() > 0)
        m_hashTableProxy->checkBoxHasChanged();
}

/* Clear/or default optional previous session UI options that may
   not be specified in the settings depending on the mode
*/
void MainWindow::restoreDefaultAttackOptions(bool shouldClearFields)
{
    if (shouldClearFields) {
        foreach(QCheckBox *widget, m_ui->optionsPage->findChildren<QCheckBox*>()) {
            widget->setChecked(false);
        }
        foreach(QComboBox *widget, m_ui->optionsPage->findChildren<QComboBox*>()) {
            widget->setEditText("");
        }
        foreach(QLineEdit *widget, m_ui->optionsPage->findChildren<QLineEdit*>()) {
            widget->setText("");
        }
    }
    int idealThreadCount = QThread::idealThreadCount();
    // john --fork will error if < 2, let's prevent it
    if (idealThreadCount < 2) {
        idealThreadCount = 2;
    }
    m_ui->spinBox_nbOfProcess->setMaximum(idealThreadCount);
    m_ui->spinBox_nbOfProcess->setValue(idealThreadCount);
    m_ui->spinBox_nbOfProcess->setMinimum(2);
    m_ui->spinBox_LimitSalts->setValue(0);
    m_ui->attackModeTabWidget->setCurrentWidget(m_ui->defaultModeTab);
    m_ui->spinBox_nbOfOpenMPThread->setValue(0); // 0 means special value = default
}

void MainWindow::checkForUpdates()
{
    QDesktopServices::openUrl(QUrl("http://openwall.info/wiki/john/johnny"));
}

void MainWindow::showHashesTableContextMenu(const QPoint& pos)
{
    QPoint globalPos = m_ui->tableView_Hashes->viewport()->mapToGlobal(pos);
    m_hashTableContextMenu->exec(globalPos);
}

void MainWindow::filterHashesTable()
{
    QRegExp regExp(QRegExp::escape(m_ui->lineEditFilter->text()), Qt::CaseInsensitive);
    m_hashTableProxy->setFilterRegExp(regExp);
}

void MainWindow::includeSelectedHashes()
{
    QModelIndexList indexes = m_ui->tableView_Hashes->selectionModel()->selectedIndexes();
    for (int i = 0; i < indexes.count(); i++) {
        if (m_hashTableProxy->data(m_hashTableProxy->index(indexes[i].row(), 0), Qt::CheckStateRole) != Qt::Checked) {
            m_hashTableProxy->setData(m_hashTableProxy->index(indexes[i].row(), 0), Qt::Checked, Qt::CheckStateRole);
        }
    }
}

void MainWindow::excludeSelectedHashes()
{
    QModelIndexList indexes = m_ui->tableView_Hashes->selectionModel()->selectedIndexes();
    for (int i = 0; i < indexes.count(); i++) {
        if (m_hashTableProxy->data(m_hashTableProxy->index(indexes[i].row(), 0), Qt::CheckStateRole) != Qt::Unchecked) {
            m_hashTableProxy->setData(m_hashTableProxy->index(indexes[i].row(), 0), UNCHECKED_PROGRAMMATICALLY, Qt::CheckStateRole);
        }
    }
    m_hashTableProxy->checkBoxHasChanged();
}

void MainWindow::setFilteringColumns()
{
    QList<int> selectedRows;
    if (m_ui->actionFilterAllColumns->isChecked())
    {
        selectedRows.append(PasswordFileModel::USER_COL);
        selectedRows.append(PasswordFileModel::PASSWORD_COL);
        selectedRows.append(PasswordFileModel::HASH_COL);
        selectedRows.append(PasswordFileModel::FORMAT_COL);
        selectedRows.append(PasswordFileModel::GECOS_COL);
    }
    else
    {
        if (m_ui->actionFilterUserColumn->isChecked())
            selectedRows.append(PasswordFileModel::USER_COL);
        if (m_ui->actionFilterPasswordColumn->isChecked())
            selectedRows.append(PasswordFileModel::PASSWORD_COL);
        if (m_ui->actionFilterHashColumn->isChecked())
            selectedRows.append(PasswordFileModel::HASH_COL);
        if (m_ui->actionFilterFormatColumn->isChecked())
            selectedRows.append(PasswordFileModel::FORMAT_COL);
        if (m_ui->actionFilterGECOSColumn->isChecked())
            selectedRows.append(PasswordFileModel::GECOS_COL);
    }

    m_hashTableProxy->setFilteredColumns(selectedRows);
}

void MainWindow::resetFilters()
{
    m_ui->lineEditFilter->clear();
    m_ui->actionFilterAllColumns->setChecked(true);
    m_ui->actionFilterUserColumn->setChecked(true);
    m_ui->actionFilterPasswordColumn->setChecked(true);
    m_ui->actionFilterHashColumn->setChecked(true);
    m_ui->actionFilterFormatColumn->setChecked(true);
    m_ui->actionFilterGECOSColumn->setChecked(true);
    m_ui->checkBoxShowOnlyCheckedHashes->setChecked(false);
    m_ui->checkBoxShowOnlyCrackedHashes->setChecked(false);
    QList<int> defaultFilteredColumns = QList<int>() << PasswordFileModel::USER_COL << PasswordFileModel::PASSWORD_COL
                                                << PasswordFileModel::HASH_COL << PasswordFileModel::FORMAT_COL
                                                << PasswordFileModel::GECOS_COL;
    m_hashTableProxy->setFilteredColumns(defaultFilteredColumns, false);
    m_hashTableProxy->setShowCrackedRowsOnly(false,false);
    m_hashTableProxy->setShowCheckedRowsOnly(false,false);
    m_hashTableProxy->setFilterRegExp("");
}