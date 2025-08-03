// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/bitcoin.h>
#include <util/perfmon.h>

#include <chainparams.h>
#include <init.h>
#include <interfaces/handler.h>
#include <interfaces/init.h>
#include <interfaces/node.h>
#include <node/ui_interface.h>
#include <noui.h>
#include <qt/bitcoingui.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/initexecutor.h>
#include <qt/intro.h>
#include <qt/networkstyle.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/splashscreen.h>
#include <qt/utilitydialog.h>
#include <qt/winshutdownmonitor.h>
#include <uint256.h>
#include <util/string.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <validation.h>

#ifdef ENABLE_WALLET
#include <qt/paymentserver.h>
#include <qt/walletcontroller.h>
#include <qt/walletmodel.h>
#endif // ENABLE_WALLET

#include <stats/stats.h>
#include <boost/signals2/connection.hpp>
#include <chrono>
#include <memory>

#include <QApplication>
#include <QDebug>
#include <QLatin1String>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QWindow>
#include <QFile>

// For crash handling to save traffic widget data
#include <signal.h>
#include <qt/trafficgraphwidget.h>
#include <qt/rpcconsole.h>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
Q_IMPORT_PLUGIN(QMacStylePlugin);
#elif defined(QT_QPA_PLATFORM_ANDROID)
Q_IMPORT_PLUGIN(QAndroidPlatformIntegrationPlugin)
#endif
#endif

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)
Q_DECLARE_METATYPE(SynchronizationState)
Q_DECLARE_METATYPE(uint256)

using node::NodeContext;

static void RegisterMetaTypes()
{
    // Register meta types used for QMetaObject::invokeMethod and Qt::QueuedConnection
    qRegisterMetaType<bool*>();
    qRegisterMetaType<SynchronizationState>();
  #ifdef ENABLE_WALLET
    qRegisterMetaType<WalletModel*>();
  #endif
    // Register typedefs (see https://doc.qt.io/qt-5/qmetatype.html#qRegisterMetaType)
    // IMPORTANT: if CAmount is no longer a typedef use the normal variant above (see https://doc.qt.io/qt-5/qmetatype.html#qRegisterMetaType-1)
    qRegisterMetaType<CAmount>("CAmount");
    qRegisterMetaType<size_t>("size_t");

    qRegisterMetaType<std::function<void()>>("std::function<void()>");
    qRegisterMetaType<QMessageBox::Icon>("QMessageBox::Icon");
    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
}

static QString GetLangTerritory()
{
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings = settings.value("language", "").toString();
    if(!lang_territory_qsettings.isEmpty())
        lang_territory = lang_territory_qsettings;
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(gArgs.GetArg("-lang", lang_territory.toStdString()));
    return lang_territory;
}

/** Set up translations */
static void initTranslations(QTranslator &qtTranslatorBase, QTranslator &qtTranslator, QTranslator &translatorBase, QTranslator &translator)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory();

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslatorBase);

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    // Load e.g. bitcoin_de.qm (shortcut "de" needs to be defined in bitcoin.qrc)
    if (translatorBase.load(lang, ":/translations/"))
        QApplication::installTranslator(&translatorBase);

    // Load e.g. bitcoin_de_DE.qm (shortcut "de_DE" needs to be defined in bitcoin.qrc)
    if (translator.load(lang_territory, ":/translations/"))
        QApplication::installTranslator(&translator);
}

static bool InitSettings()
{
    if (!gArgs.GetSettingsPath()) {
        return true; // Do nothing if settings file disabled.
    }

    std::vector<std::string> errors;
    if (!gArgs.ReadSettingsFile(&errors)) {
        std::string error = QT_TRANSLATE_NOOP("bitcoin-core", "Settings file could not be read");
        std::string error_translated = QCoreApplication::translate("bitcoin-core", error.c_str()).toStdString();
        InitError(Untranslated(strprintf("%s:\n%s\n", error, MakeUnorderedList(errors))));

        QMessageBox messagebox(QMessageBox::Critical, PACKAGE_NAME, QString::fromStdString(strprintf("%s.", error_translated)), QMessageBox::Reset | QMessageBox::Abort);
        /*: Explanatory text shown on startup when the settings file cannot be read.
            Prompts user to make a choice between resetting or aborting. */
        messagebox.setInformativeText(QObject::tr("Do you want to reset settings to default values, or to abort without making changes?"));
        messagebox.setDetailedText(QString::fromStdString(MakeUnorderedList(errors)));
        messagebox.setTextFormat(Qt::PlainText);
        messagebox.setDefaultButton(QMessageBox::Reset);
        switch (messagebox.exec()) {
        case QMessageBox::Reset:
            break;
        case QMessageBox::Abort:
            return false;
        default:
            assert(false);
        }
    }

    errors.clear();
    if (!gArgs.WriteSettingsFile(&errors)) {
        std::string error = QT_TRANSLATE_NOOP("bitcoin-core", "Settings file could not be written");
        std::string error_translated = QCoreApplication::translate("bitcoin-core", error.c_str()).toStdString();
        InitError(Untranslated(strprintf("%s:\n%s\n", error, MakeUnorderedList(errors))));

        QMessageBox messagebox(QMessageBox::Critical, PACKAGE_NAME, QString::fromStdString(strprintf("%s.", error_translated)), QMessageBox::Ok);
        /*: Explanatory text shown on startup when the settings file could not be written.
            Prompts user to check that we have the ability to write to the file.
            Explains that the user has the option of running without a settings file.*/
        messagebox.setInformativeText(QObject::tr("A fatal error occurred. Check that settings file is writable, or try running with -nosettings."));
        messagebox.setDetailedText(QString::fromStdString(MakeUnorderedList(errors)));
        messagebox.setTextFormat(Qt::PlainText);
        messagebox.setDefaultButton(QMessageBox::Ok);
        messagebox.exec();
        return false;
    }
    return true;
}

/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString &msg)
{
    Q_UNUSED(context);
    if (type == QtDebugMsg) {
        LogPrint(BCLog::QT, "GUI: %s\n", msg.toStdString());
    } else {
        LogPrintf("GUI: %s\n", msg.toStdString());
    }
}

static int qt_argc = 1;
static const char* qt_argv = "bitcoin-qt";

// Helper function to detect dark mode across different platforms
bool isSystemDarkMode() {
#ifdef Q_OS_LINUX
    // Check environment variables first
    QString gtkTheme = qgetenv("GTK_THEME");
    QString qtTheme = qgetenv("QT_STYLE_OVERRIDE");

    if (gtkTheme.contains("dark", Qt::CaseInsensitive) ||
        qtTheme.contains("dark", Qt::CaseInsensitive)) {
        return true;
    }

    // Check XFCE specific settings
    QString xfceTheme = qgetenv("XFCE_THEME");
    if (xfceTheme.contains("dark", Qt::CaseInsensitive)) {
        return true;
    }

    // Try to detect via gsettings (GNOME)
    QProcess gsettingsProcess;
    gsettingsProcess.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
    if (gsettingsProcess.waitForFinished(1000)) {
        QString output = gsettingsProcess.readAllStandardOutput().trimmed();
        if (output.contains("dark", Qt::CaseInsensitive)) {
            return true;
        }
    }

    // Try to detect via kreadconfig5 (KDE)
    QProcess kdeProcess;
    kdeProcess.start("kreadconfig5", {"--file", "kcmdisplayrc", "--group", "General", "--key", "ColorScheme"});
    if (kdeProcess.waitForFinished(1000)) {
        QString output = kdeProcess.readAllStandardOutput().trimmed();
        if (output.contains("dark", Qt::CaseInsensitive)) {
            return true;
        }
    }

    // Check XFCE settings file
    QString xfceConfig = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml";
    if (QFile::exists(xfceConfig)) {
        QFile file(xfceConfig);
        if (file.open(QIODevice::ReadOnly)) {
            QString content = file.readAll();
            if (content.contains("dark", Qt::CaseInsensitive) ||
                content.contains("Adwaita-dark", Qt::CaseInsensitive) ||
                content.contains("Breeze-Dark", Qt::CaseInsensitive)) {
                return true;
            }
        }
    }
#endif

#ifdef Q_OS_WIN
    // Windows registry check for dark mode
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", QSettings::NativeFormat);
    return settings.value("AppsUseLightTheme", 1).toInt() == 0;
#endif

#ifdef Q_OS_MAC
    // macOS appearance check
    QProcess process;
    process.start("defaults", {"read", "-g", "AppleInterfaceStyle"});
    if (process.waitForFinished(1000)) {
        QString output = process.readAllStandardOutput().trimmed();
        return output.contains("Dark", Qt::CaseInsensitive);
    }
#endif

    // Fallback: try to detect from current palette (less reliable)
    QPalette currentPalette = QApplication::palette();
    QColor windowColor = currentPalette.color(QPalette::Window);

    // Use both lightness and value for better detection
    int lightness = windowColor.lightness();
    int value = windowColor.value();

    // If both lightness and value are low, it's likely dark mode
    return (lightness < 128 && value < 128);
}

BitcoinApplication::BitcoinApplication():
    QApplication(qt_argc, const_cast<char **>(&qt_argv)),
    optionsModel(nullptr),
    clientModel(nullptr),
    window(nullptr),
    pollShutdownTimer(nullptr),
    returnValue(0),
    platformStyle(nullptr)
{
    // Qt runs setlocale(LC_ALL, "") on initialization.
    RegisterMetaTypes();
    setQuitOnLastWindowClosed(false);

    // Set up performance monitoring
    setupPerfMonitoring();

    // Set up Qt6-like dark theme if system is in dark mode
    if (isSystemDarkMode()) {
        setStyle("Fusion");
        QPalette darkPalette;
        darkPalette.setColor(QPalette::Window, QColor(53, 53, 53));
        darkPalette.setColor(QPalette::WindowText, Qt::white);
        darkPalette.setColor(QPalette::Base, QColor(25, 25, 25));
        darkPalette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        darkPalette.setColor(QPalette::Text, Qt::white);
        // Set disabled text color for menu items
        QColor disabledTextColor = QColor(100, 100, 100);  // A darker gray that's clearly disabled
        darkPalette.setColor(QPalette::Disabled, QPalette::Text, disabledTextColor);
        darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, disabledTextColor);
        darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledTextColor);
        // Remove text shadow effect for disabled items
        darkPalette.setColor(QPalette::Disabled, QPalette::Light, disabledTextColor);
        darkPalette.setColor(QPalette::Disabled, QPalette::Midlight, disabledTextColor);
        darkPalette.setColor(QPalette::Disabled, QPalette::Dark, disabledTextColor);
        darkPalette.setColor(QPalette::Disabled, QPalette::Mid, disabledTextColor);
        darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
        darkPalette.setColor(QPalette::ButtonText, Qt::white);
        darkPalette.setColor(QPalette::BrightText, Qt::red);
        darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
        darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        darkPalette.setColor(QPalette::HighlightedText, Qt::black);
        setPalette(darkPalette);
    }
}

void BitcoinApplication::setupPlatformStyle()
{
    // UI per-platform customization
    // This must be done inside the BitcoinApplication constructor, or after it, because
    // PlatformStyle::instantiate requires a QApplication
    std::string platformName;
    platformName = gArgs.GetArg("-uiplatform", BitcoinGUI::DEFAULT_UIPLATFORM);
    platformStyle = PlatformStyle::instantiate(QString::fromStdString(platformName));
    if (!platformStyle) // Fall back to "other" if specified name not found
        platformStyle = PlatformStyle::instantiate("other");
    assert(platformStyle);
}

BitcoinApplication::~BitcoinApplication()
{
    m_executor.reset();

    delete window;
    window = nullptr;
    delete platformStyle;
    platformStyle = nullptr;
}

#ifdef ENABLE_WALLET
void BitcoinApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
}
#endif

void BitcoinApplication::createOptionsModel(bool resetSettings)
{
    PERF_MONITOR("qt_create_options_model");
    optionsModel = new OptionsModel(this, resetSettings);
}

void BitcoinApplication::createWindow(const NetworkStyle *networkStyle)
{
    window = new BitcoinGUI(node(), platformStyle, networkStyle, nullptr);
    connect(window, &BitcoinGUI::quitRequested, this, &BitcoinApplication::requestShutdown);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, &QTimer::timeout, [this]{
        if (!QApplication::activeModalWidget()) {
            window->detectShutdown();
        }
    });
}

void BitcoinApplication::createSplashScreen(const NetworkStyle *networkStyle)
{
    assert(!m_splash);
    m_splash = new SplashScreen(networkStyle);
    // We don't hold a direct pointer to the splash screen after creation, but the splash
    // screen will take care of deleting itself when finish() happens.
    m_splash->show();
    connect(this, &BitcoinApplication::splashFinished, m_splash, &SplashScreen::finish);
    connect(this, &BitcoinApplication::requestedShutdown, m_splash, &QWidget::close);
}

void BitcoinApplication::createNode(interfaces::Init& init)
{
    assert(!m_node);
    m_node = init.makeNode();
    if (optionsModel) optionsModel->setNode(*m_node);
    if (m_splash) m_splash->setNode(*m_node);
}

bool BitcoinApplication::baseInitialize()
{
    return node().baseInitialize();
}

void BitcoinApplication::startThread()
{
    assert(!m_executor);
    m_executor.emplace(node());

    /*  communication to and from thread */
    connect(&m_executor.value(), &InitExecutor::initializeResult, this, &BitcoinApplication::initializeResult);
    connect(&m_executor.value(), &InitExecutor::shutdownResult, this, &QCoreApplication::quit);
    connect(&m_executor.value(), &InitExecutor::runawayException, this, &BitcoinApplication::handleRunawayException);
    connect(this, &BitcoinApplication::requestedInitialize, &m_executor.value(), &InitExecutor::initialize);
    connect(this, &BitcoinApplication::requestedShutdown, &m_executor.value(), &InitExecutor::shutdown);
}

void BitcoinApplication::parameterSetup()
{
    // Default printtoconsole to false for the GUI. GUI programs should not
    // print to the console unnecessarily.
    gArgs.SoftSetBoolArg("-printtoconsole", false);

    InitLogging(gArgs);
    InitParameterInteraction(gArgs);
}

void BitcoinApplication::InitPruneSetting(int64_t prune_MiB)
{
    optionsModel->SetPruneTargetGB(PruneMiBtoGB(prune_MiB), true);
}

void BitcoinApplication::requestInitialize()
{
    PERF_MONITOR("qt_request_initialize");
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    Q_EMIT requestedInitialize();
}

void BitcoinApplication::requestShutdown()
{
    PERF_MONITOR("qt_request_shutdown");
    for (const auto w : QGuiApplication::topLevelWindows()) {
        w->hide();
    }

    // Show a simple window indicating shutdown status
    // Do this first as some of the steps may take some time below,
    // for example the RPC console may still be executing a command.
    shutdownWindow.reset(ShutdownWindow::showShutdownWindow(window));

    qDebug() << __func__ << ": Requesting shutdown";

    // Must disconnect node signals otherwise current thread can deadlock since
    // no event loop is running.
    window->unsubscribeFromCoreSignals();
    // Request node shutdown, which can interrupt long operations, like
    // rescanning a wallet.
    node().startShutdown();
    // Unsetting the client model can cause the current thread to wait for node
    // to complete an operation, like wait for a RPC execution to complete.
    window->setClientModel(nullptr);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    // Delete wallet controller here manually, instead of relying on Qt object
    // tracking (https://doc.qt.io/qt-5/objecttrees.html). This makes sure
    // walletmodel m_handle_* notification handlers are deleted before wallets
    // are unloaded, which can simplify wallet implementations. It also avoids
    // these notifications having to be handled while GUI objects are being
    // destroyed, making GUI code less fragile as well.
    delete m_wallet_controller;
    m_wallet_controller = nullptr;
#endif // ENABLE_WALLET

    delete clientModel;
    clientModel = nullptr;

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void BitcoinApplication::initializeResult(bool success, interfaces::BlockAndHeaderTipInfo tip_info)
{
    PERF_MONITOR("qt_initialize_result");
    qDebug() << __func__ << ": Initialization result: " << success;
    // Set exit result.
    returnValue = success ? EXIT_SUCCESS : EXIT_FAILURE;
    if(success)
    {
        // Log this only after AppInitMain finishes, as then logging setup is guaranteed complete
        qInfo() << "Platform customization:" << platformStyle->getName();
        clientModel = new ClientModel(node(), optionsModel);
        window->setClientModel(clientModel, &tip_info);
#ifdef ENABLE_WALLET
        if (WalletModel::isWalletEnabled()) {
            m_wallet_controller = new WalletController(*clientModel, platformStyle, this);
            window->setWalletController(m_wallet_controller);
            if (paymentServer) {
                paymentServer->setOptionsModel(optionsModel);
            }
        }
#endif // ENABLE_WALLET

        // If -min option passed, start window minimized (iconified) or minimized to tray
        if (!gArgs.GetBoolArg("-min", false)) {
            window->show();
        } else if (clientModel->getOptionsModel()->getMinimizeToTray() && window->hasTrayIcon()) {
            // do nothing as the window is managed by the tray icon
        } else {
            window->showMinimized();
        }
        Q_EMIT splashFinished();
        Q_EMIT windowShown(window);

        // Test our responsive processEvents wrapper
        LogPrint(BCLog::QT, "Testing responsive processEvents wrapper...\n");
        static_cast<BitcoinApplication*>(qApp)->processEvents();
        LogPrint(BCLog::QT, "Test completed\n");

#ifdef ENABLE_WALLET
        // Now that initialization/startup is done, process any command-line
        // bitcoin: URIs or payment requests:
        if (paymentServer) {
            connect(paymentServer, &PaymentServer::receivedPaymentRequest, window, &BitcoinGUI::handlePaymentRequest);
            connect(window, &BitcoinGUI::receivedURI, paymentServer, &PaymentServer::handleURIOrFile);
            connect(paymentServer, &PaymentServer::message, [this](const QString& title, const QString& message, unsigned int style) {
                window->message(title, message, style);
            });
            QTimer::singleShot(100ms, paymentServer, &PaymentServer::uiReady);
        }
#endif
        pollShutdownTimer->start(SHUTDOWN_POLLING_DELAY);
    } else {
        Q_EMIT splashFinished(); // Make sure splash screen doesn't stick around during shutdown
        requestShutdown();
    }
}

void BitcoinApplication::handleRunawayException(const QString &message)
{
    QMessageBox::critical(
        nullptr, tr("Runaway exception"),
        tr("A fatal error occurred. %1 can no longer continue safely and will quit.").arg(PACKAGE_NAME) +
        QLatin1String("<br><br>") + GUIUtil::MakeHtmlLink(message, PACKAGE_BUGREPORT));
    ::exit(EXIT_FAILURE);
}

void BitcoinApplication::handleNonFatalException(const QString& message)
{
    assert(QThread::currentThread() == thread());
    QMessageBox::warning(
        nullptr, tr("Internal error"),
        tr("An internal error occurred. %1 will attempt to continue safely. This is "
           "an unexpected bug which can be reported as described below.").arg(PACKAGE_NAME) +
        QLatin1String("<br><br>") + GUIUtil::MakeHtmlLink(message, PACKAGE_BUGREPORT));
}

WId BitcoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

bool BitcoinApplication::event(QEvent* e)
{
    QElapsedTimer handlerTimer;
    handlerTimer.start();

    if (e->type() == QEvent::Quit) {
        requestShutdown();
        return true;
    }

    switch (e->type())
    {
    case QEvent::MouseButtonPress:
        LogPrint(BCLog::QT, "Mouse button press event - requesting responsiveness\n");
        if (m_node && window) {
            ClientModel* clientModel = window->getClientModel();
            if (clientModel) {
                clientModel->requestResponsiveness("Mouse button press");
                // DISABLED: Condition variable signaling was causing issues
                QTimer::singleShot(10, [clientModel]() {
                    LogPrint(BCLog::QT, "Releasing responsiveness after mouse press\n");
                    clientModel->releaseResponsiveness();
                });
            }
        }
        break;
    case QEvent::MouseButtonRelease:
        LogPrint(BCLog::QT, "Mouse button release event - requesting responsiveness\n");
        if (m_node && window) {
            ClientModel* clientModel = window->getClientModel();
            if (clientModel) {
                clientModel->requestResponsiveness("Mouse button release");
                QTimer::singleShot(10, [clientModel]() {
                    LogPrint(BCLog::QT, "Releasing responsiveness after mouse release\n");
                    clientModel->releaseResponsiveness();
                });
            }
        }
        break;
    case QEvent::MouseButtonDblClick:
        LogPrint(BCLog::QT, "Mouse double-click event - requesting responsiveness\n");
        if (m_node && window) {
            ClientModel* clientModel = window->getClientModel();
            if (clientModel) {
                clientModel->requestResponsiveness("Mouse double-click");
                QTimer::singleShot(10, [clientModel]() {
                    LogPrint(BCLog::QT, "Releasing responsiveness after mouse double-click\n");
                    clientModel->releaseResponsiveness();
                });
            }
        }
        break;
    case QEvent::KeyPress:
        LogPrint(BCLog::QT, "Key press event - requesting responsiveness\n");
        if (m_node && window) {
            ClientModel* clientModel = window->getClientModel();
            if (clientModel) {
                clientModel->requestResponsiveness("Key press");
                QTimer::singleShot(10, [clientModel]() {
                    LogPrint(BCLog::QT, "Releasing responsiveness after key press\n");
                    clientModel->releaseResponsiveness();
                });
            }
        }
        break;
    case QEvent::KeyRelease:
        LogPrint(BCLog::QT, "Key release event - requesting responsiveness\n");
        if (m_node && window) {
            ClientModel* clientModel = window->getClientModel();
            if (clientModel) {
                clientModel->requestResponsiveness("Key release");
                QTimer::singleShot(10, [clientModel]() {
                    LogPrint(BCLog::QT, "Releasing responsiveness after key release\n");
                    clientModel->releaseResponsiveness();
                });
            }
        }
        break;
    case QEvent::Wheel:
        LogPrint(BCLog::QT, "Mouse wheel event - requesting responsiveness\n");
        if (m_node && window) {
            ClientModel* clientModel = window->getClientModel();
            if (clientModel) {
                clientModel->requestResponsiveness("Mouse wheel");
                QTimer::singleShot(10, [clientModel]() {
                    LogPrint(BCLog::QT, "Releasing responsiveness after mouse wheel\n");
                    clientModel->releaseResponsiveness();
                });
            }
        }
        break;
    default:
        break;
    }

    qint64 handlerTime = handlerTimer.nsecsElapsed() / 1000000;
    if (handlerTime > 1000) { // Much higher threshold
        qDebug() << "[EVENT_HANDLER] Bitcoin event handler took" << handlerTime << "ms for event" << e->type();
    }

    // Safety: Add timeout protection for event handlers
    if (handlerTime > 30000) {
        qDebug() << "[EVENT_HANDLER] EMERGENCY: Bitcoin event handler frozen for" << handlerTime << "ms!";
    }

    return QApplication::event(e);
}

void BitcoinApplication::processEvents()
{
    LogPrint(BCLog::QT, "BitcoinApplication::processEvents() called - requesting responsiveness\n");

    // Request responsiveness before processing events
    if (m_node && window) {
        ClientModel* clientModel = window->getClientModel();
        if (clientModel) {
            LogPrint(BCLog::QT, "Requesting responsiveness from ClientModel\n");
            clientModel->requestResponsiveness("Process events");
        } else {
            LogPrint(BCLog::QT, "No ClientModel available for responsiveness\n");
        }
    } else {
        LogPrint(BCLog::QT, "No m_node or window available for responsiveness\n");
    }

    LogPrint(BCLog::QT, "Calling QApplication::processEvents()\n");
    QApplication::processEvents();

    // Release responsiveness after processing events
    if (m_node && window) {
        ClientModel* clientModel = window->getClientModel();
        if (clientModel) {
            LogPrint(BCLog::QT, "Releasing responsiveness from ClientModel\n");
            clientModel->releaseResponsiveness();
        }
    }

    LogPrint(BCLog::QT, "BitcoinApplication::processEvents() completed\n");
}

bool BitcoinApplication::notify(QObject *receiver, QEvent *event)
{
    // Bitcoin-specific event timing with safety checks
    static QElapsedTimer eventTimer;
    static bool firstEvent = true;
    static int eventCount = 0;

    // Safety: Limit event processing overhead
    eventCount++;
    if (eventCount > 1000) { // Reset counter periodically
        eventCount = 0;
    }

    if (firstEvent) {
        eventTimer.start();
        firstEvent = false;
    }

    qint64 elapsed = eventTimer.nsecsElapsed() / 1000000; // Convert to milliseconds
    eventTimer.restart();

    // ONLY process Bitcoin-specific events
    bool isBitcoinEvent = false;
    QString className = "NULL";

    if (receiver && receiver->metaObject()) {
        className = receiver->metaObject()->className();

        // Debug: Log all classes being intercepted (with rate limiting)
        static int debugCounter = 0;
        debugCounter++;
        if (debugCounter % 100 == 0) { // Log every 100th event to avoid spam
            qDebug() << "[DEBUG] notify() intercepting event" << event->type()
                     << "to class:" << className;
        }

        // Debug: Log specific event types we're interested in
        if (event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseButtonRelease ||
            event->type() == QEvent::KeyPress ||
            event->type() == QEvent::KeyRelease) {
            qDebug() << "[DEBUG] User input event intercepted:" << event->type()
                     << "to class:" << className;
        }

        isBitcoinEvent = className.contains("Bitcoin") ||
                        className.contains("ClientModel") ||
                        className.contains("PeerTable") ||
                        className.contains("BanTable") ||
                        className.contains("Wallet") ||
                        className.contains("Transaction") ||
                        className.contains("SendCoins") ||
                        className.contains("ReceiveCoins") ||
                        className.contains("AddressBook") ||
                        className.contains("RPCConsole") ||
                        className.contains("OptionsDialog") ||
                        className.contains("AboutDialog") ||
                        className.contains("HelpMessageDialog") ||
                        className.contains("ModalOverlay") ||
                        className.contains("TrafficGraph") ||
                        className.contains("UnitDisplayStatusBarControl") ||
                        className.contains("QMenu") || // Bitcoin menus
                        className.contains("QAction"); // Bitcoin actions

        // Debug: Log when we identify a Bitcoin event
        if (isBitcoinEvent) {
            qDebug() << "[DEBUG] Identified Bitcoin event:" << event->type()
                     << "to class:" << className;
        }
    } else {
        // Debug: Log NULL receiver events
        static int nullCounter = 0;
        nullCounter++;
        if (nullCounter % 50 == 0) { // Log every 50th NULL event
            qDebug() << "[DEBUG] notify() intercepting event" << event->type() << "to NULL receiver";
        }
    }

    // Log ALL events with significant delay (no Bitcoin filtering for now)
    if (elapsed > 1000) {
        qDebug() << "[EVENT_TIMED] Event delay:" << elapsed << "ms for event" << event->type()
                 << "to class:" << className;
    }

    // Critical warning for very long delays (any event)
    if (elapsed > 10000) {
        qDebug() << "[EVENT_TIMED] CRITICAL: Event loop stalled for" << elapsed << "ms! Event:" << event->type()
                 << "to class:" << className;
    }

    // Safety: Add timeout protection
    if (elapsed > 30000) {
        qDebug() << "[EVENT_TIMED] EMERGENCY: Event loop frozen for" << elapsed << "ms! Event:" << event->type()
                 << "to class:" << className << "! Aborting processing.";
        return false; // Don't process this event
    }

    return QApplication::notify(receiver, event);
}

static void SetupUIArgs(ArgsManager& argsman)
{
    argsman.AddArg("-choosedatadir", strprintf("Choose data directory on startup (default: %u)", DEFAULT_CHOOSE_DATADIR), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-lang=<lang>", "Set language, for example \"de_DE\" (default: system locale)", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-min", "Start minimized", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-resetguisettings", "Reset all settings changed in the GUI", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-splash", strprintf("Show splash screen on startup (default: %u)", DEFAULT_SPLASHSCREEN), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-uiplatform", strprintf("Select platform to customize UI for (one of windows, macosx, other; default: %s)", BitcoinGUI::DEFAULT_UIPLATFORM), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
}

static void handleCrash(int sig)
{
    // Save traffic widget data if it exists
    if (QApplication::instance()) {
        // Find the traffic graph widget directly from the application
        TrafficGraphWidget* trafficGraph = QApplication::instance()->findChild<TrafficGraphWidget*>();
        if (trafficGraph) {
            // Use QMetaObject to call the private saveData method
            QMetaObject::invokeMethod(trafficGraph, "saveData", Qt::DirectConnection);
        }
    }

    // Call the original signal handler
    signal(sig, SIG_DFL);
    raise(sig);
}

int GuiMain(int argc, char* argv[])
{
    // Set up signal handlers for crashes
    signal(SIGSEGV, handleCrash);
    signal(SIGABRT, handleCrash);
    signal(SIGFPE, handleCrash);
    signal(SIGILL, handleCrash);

#ifdef WIN32
    util::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif

    std::unique_ptr<interfaces::Init> init = interfaces::MakeGuiInit(argc, argv);

    SetupEnvironment();
    util::ThreadSetInternalName("main");

    // Subscribe to global signals from core
    boost::signals2::scoped_connection handler_message_box = ::uiInterface.ThreadSafeMessageBox_connect(noui_ThreadSafeMessageBox);
    boost::signals2::scoped_connection handler_question = ::uiInterface.ThreadSafeQuestion_connect(noui_ThreadSafeQuestion);
    boost::signals2::scoped_connection handler_init_message = ::uiInterface.InitMessage_connect(noui_InitMessage);

    // Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

    /// 1. Basic Qt initialization (not dependent on parameters or configuration)
    Q_INIT_RESOURCE(bitcoin);
    Q_INIT_RESOURCE(bitcoin_locale);

    // Generate high-dpi pixmaps
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

#if defined(QT_QPA_PLATFORM_ANDROID)
    QApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif

    BitcoinApplication app;
    GUIUtil::LoadFont(QStringLiteral(":/fonts/monospace"));

    /// 2. Parse command-line options. We do this after qt in order to show an error if there are problems parsing these
    // Command-line options take precedence:
    SetupServerArgs(gArgs);
    SetupUIArgs(gArgs);
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        InitError(strprintf(Untranslated("Error parsing command line arguments: %s\n"), error));
        // Create a message box, because the gui has neither been created nor has subscribed to core signals
        QMessageBox::critical(nullptr, PACKAGE_NAME,
            // message cannot be translated because translations have not been initialized
            QString::fromStdString("Error parsing command line arguments: %1.").arg(QString::fromStdString(error)));
        return EXIT_FAILURE;
    }

    // Now that the QApplication is setup and we have parsed our parameters, we can set the platform style
    app.setupPlatformStyle();

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (HelpRequested(gArgs) || gArgs.IsArgSet("-version")) {
        HelpMessageDialog help(nullptr, gArgs.IsArgSet("-version"));
        help.showOrPrint();
        return EXIT_SUCCESS;
    }

    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    bool did_show_intro = false;
    int64_t prune_MiB = 0;  // Intro dialog prune configuration
    // Gracefully exit if the user cancels
    if (!Intro::showIfNeeded(did_show_intro, prune_MiB)) return EXIT_SUCCESS;

    /// 6. Determine availability of data directory and parse bitcoin.conf
    /// - Do not call gArgs.GetDataDirNet() before this step finishes
    if (!CheckDataDirOption()) {
        InitError(strprintf(Untranslated("Specified data directory \"%s\" does not exist.\n"), gArgs.GetArg("-datadir", "")));
        QMessageBox::critical(nullptr, PACKAGE_NAME,
            QObject::tr("Error: Specified data directory \"%1\" does not exist.").arg(QString::fromStdString(gArgs.GetArg("-datadir", ""))));
        return EXIT_FAILURE;
    }
    if (!gArgs.ReadConfigFiles(error, true)) {
        InitError(strprintf(Untranslated("Error reading configuration file: %s\n"), error));
        QMessageBox::critical(nullptr, PACKAGE_NAME,
            QObject::tr("Error: Cannot parse configuration file: %1.").arg(QString::fromStdString(error)));
        return EXIT_FAILURE;
    }

    /// 7. Determine network (and switch to network specific options)
    // - Do not call Params() before this step
    // - Do this after parsing the configuration file, as the network can be switched there
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel

    // Check for chain settings (Params() calls are only valid after this clause)
    try {
        SelectParams(gArgs.GetChainName());
    } catch(std::exception &e) {
        InitError(Untranslated(strprintf("%s\n", e.what())));
        QMessageBox::critical(nullptr, PACKAGE_NAME, QObject::tr("Error: %1").arg(e.what()));
        return EXIT_FAILURE;
    }
#ifdef ENABLE_WALLET
    // Parse URIs on command line -- this can affect Params()
    PaymentServer::ipcParseCommandLine(argc, argv);
#endif

    if (!InitSettings()) {
        return EXIT_FAILURE;
    }

    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(Params().NetworkIDString()));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(networkStyle->getAppName());
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);

#ifdef ENABLE_WALLET
    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        exit(EXIT_SUCCESS);

    // Start up the payment server early, too, so impatient users that click on
    // bitcoin: links repeatedly have their payment requests routed to this process:
    if (WalletModel::isWalletEnabled()) {
        app.createPaymentServer();
    }
#endif // ENABLE_WALLET

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that out-of-focus labels do not contain text cursor.
    app.installEventFilter(new GUIUtil::LabelOutOfFocusEventFilter(&app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    qApp->installNativeEventFilter(new WinShutdownMonitor());
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
    // Allow parameter interaction before we create the options model
    app.parameterSetup();
    GUIUtil::LogQtInfo();
    // Load GUI settings from QSettings
    app.createOptionsModel(gArgs.GetBoolArg("-resetguisettings", false));

    if (did_show_intro) {
        // Store intro dialog settings other than datadir (network specific)
        app.InitPruneSetting(prune_MiB);
    }

    // Enable mempool stats by default
    gArgs.SoftSetBoolArg("-statsenable", true);

    if (gArgs.GetBoolArg("-splash", DEFAULT_SPLASHSCREEN) && !gArgs.GetBoolArg("-min", false))
        app.createSplashScreen(networkStyle.data());

    app.createNode(*init);

    int rv = EXIT_SUCCESS;
    try
    {
        app.createWindow(networkStyle.data());
        // Perform base initialization before spinning up initialization/shutdown thread
        // This is acceptable because this function only contains steps that are quick to execute,
        // so the GUI thread won't be held up.
        if (app.baseInitialize()) {
            app.requestInitialize();
#if defined(Q_OS_WIN)
            WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("%1 didn't yet exit safely…").arg(PACKAGE_NAME), (HWND)app.getMainWinId());
#endif
            app.exec();
            rv = app.getReturnValue();
        } else {
            // A dialog with detailed error will have been shown by InitError()
            rv = EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(app.node().getWarnings().translated));
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(app.node().getWarnings().translated));
    }
    return rv;
}

// Add periodic stats logging
void BitcoinApplication::setupPerfMonitoring()
{
    QTimer* perfTimer = new QTimer(this);
    perfTimer->setInterval(60000); // Log stats every minute
    connect(perfTimer, &QTimer::timeout, []() {
        LogPerfStats();
        PerfMonitor::Instance().Reset(); // Reset stats after logging
    });
    perfTimer->start();
}
