// Offline fixture links the production UI, policy and DriverClient packet code.
// The sole transport and SCM implementations below are fakes: no device is opened,
// no hypervisor is entered and no real service is changed.
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QElapsedTimer>
#include <QDialog>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include "../../Ksword5.1/Ksword5.1/theme.h"
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QShowEvent>
#include <memory>
#include <cstring>
#include <iostream>
#include <vector>
#include "../../Ksword5.1/Ksword5.1/UI/HvmControl.h"
#include "../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include "../../Ksword5.1/Ksword5.1/ksword/service/service.h"
#include "../../Ksword5.1/Ksword5.1/Framework/DestructiveActionConfirmation.h"
#include "../../shared/driver/KswordArkHvmRequest.h"
// Inspect rendered widgets and inject snapshots without a production test API.
#define private public
#include "../../Ksword5.1/Ksword5.1/UI/HvmGuestVmPanel.h"
#include "../../Ksword5.1/Ksword5.1/UI/HvmWatchPanel.h"
#include "../../Ksword5.1/Ksword5.1/HvmDock/HvmDock.h"
#include "../../Ksword5.1/Ksword5.1/KernelDock/KernelHvmTab.h"
#undef private
#include "../../Ksword5.1/Ksword5.1/UI/ThemeStatusRole.h"

namespace {
KSWORD_ARK_QUERY_HVM_RESPONSE snapshot{};
std::unique_ptr<KSWORD_ARK_HVM_METRICS_RESPONSE> metrics;
std::vector<KSWORD_ARK_CONTROL_HVM_REQUEST> commands;
unsigned long failCommand = 0, badQuery = 0, badMetrics = 0;
bool bumpAtControl = false;
int checks = 0, failures = 0, scmMutations = 0;
void check(bool value, const char* description) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
void reset(unsigned long backend = KSWORD_ARK_HVM_BACKEND_SVM) {
    snapshot = {};
    snapshot.version = KSWORD_ARK_HVM_PROTOCOL_VERSION;
    snapshot.size = sizeof(snapshot);
    snapshot.backend = backend;
    snapshot.generation = 73;
    snapshot.processorCount = 2;
    snapshot.residentImplementation = KSWORD_ARK_HVM_IMPLEMENTATION_CAPABILITY_ONLY;
    snapshot.nestedImplementation = KSWORD_ARK_HVM_IMPLEMENTATION_CAPABILITY_ONLY;
    snapshot.featureFlags = KswordArkHvmHardwareFeatures(backend) |
        KSWORD_ARK_HVM_FEATURE_RESIDENT_VMM | KSWORD_ARK_HVM_FEATURE_RESIDENT_LIFECYCLE_GUARDED;
    if (backend == KSWORD_ARK_HVM_BACKEND_SVM) {
        snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_DISPATCH;
        snapshot.svmCapabilities.asidCount = 64;
        snapshot.svmCapabilities.msrValidMask = 15;
    } else { snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_MSR_BITMAP; }
    snapshot.processors[1].processorNumber = 1;
    commands.clear(); failCommand = badQuery = badMetrics = 0; bumpAtControl = false;
    ksword::hvm::setNestedAllowed(false);
    ksword::hvm::setNestedDispatchEnabled(false);
    ksword::hvm::setWriteAccessEnabled(true);
    ksword::hvm::setLocalEptEnabled(false);
    ksword::hvm::setEptpSwitchEnabled(false);
    ksword::hvm::setVeEnabled(false);
    ksword::hvm::setVmFuncEnabled(false);
    ksword::hvm::setHypervisorHidden(false);
    metrics = std::make_unique<KSWORD_ARK_HVM_METRICS_RESPONSE>();
    metrics->version = KSWORD_ARK_HVM_METRICS_VERSION;
    metrics->size = sizeof(*metrics); metrics->qpcFrequency = 10000000;
    metrics->backend = backend; metrics->svmProcessorCount = 2;
    for (unsigned long i = 0; i < 2; ++i) {
        auto& row = metrics->svmProcessors[i]; row.number = static_cast<unsigned char>(i);
        row.generation = snapshot.generation; row.stage = KSWORD_ARK_HVM_STAGE_PREPARED;
        row.vmcbPa = 0x1000 + i * 0x1000; row.hsavePa = 0x4000 + i * 0x1000; row.nptRootPa = 0x8000;
    }
}
ksword::ark::HvmStatusResult status() {
    ksword::ark::HvmStatusResult r{}; r.io.ok = true; r.response = snapshot; return r;
}
}

namespace ksword::ark {
IoResult DriverClient::deviceIoControl(unsigned long code, void* input, unsigned long,
    void* output, unsigned long bytes, DriverHandle*) const {
    IoResult io{}; io.ok = true; io.bytesReturned = bytes;
    if (code == IOCTL_KSWORD_ARK_QUERY_HVM) {
        auto& r = *static_cast<KSWORD_ARK_QUERY_HVM_RESPONSE*>(output); r = snapshot;
        if (badQuery == 1) { --io.bytesReturned; }
        if (badQuery == 2) { ++r.version; }
        if (badQuery == 3) { --r.size; }
        if (badQuery == 4) { r.processorCount = KSWORD_ARK_HVM_MAX_PROCESSORS + 1; }
    } else if (code == IOCTL_KSWORD_ARK_HVM_METRICS) {
        auto& r = *static_cast<KSWORD_ARK_HVM_METRICS_RESPONSE*>(output); r = *metrics;
        if (badMetrics == 1) { --io.bytesReturned; }
        if (badMetrics == 2) { ++r.version; }
        if (badMetrics == 3) { --r.size; }
        if (badMetrics == 4) { r.svmProcessorCount = KSWORD_ARK_HVM_MAX_PROCESSORS + 1; }
        if (badMetrics == 5) { r.qpcFrequency = 0; }
    } else if (code == IOCTL_KSWORD_ARK_CONTROL_HVM) {
        const auto& request = *static_cast<KSWORD_ARK_CONTROL_HVM_REQUEST*>(input);
        commands.push_back(request);
        auto& r = *static_cast<KSWORD_ARK_CONTROL_HVM_RESPONSE*>(output); r = {};
        if (bumpAtControl) { ++snapshot.generation; bumpAtControl = false; }
        if (request.expectedGeneration != snapshot.generation || request.command == failCommand) {
            r.status = KSWORD_ARK_HVM_CONTROL_STATUS_INVALID_REQUEST; r.lastStatus = static_cast<long>(0xC0000184UL);
            return io;
        }
        ++snapshot.generation;
        for (unsigned long i = 0; i < metrics->svmProcessorCount; ++i) { metrics->svmProcessors[i].generation = snapshot.generation; }
        if (request.command == KSWORD_ARK_HVM_CONTROL_PREPARE) {
            snapshot.stateFlags |= KSWORD_ARK_HVM_STATE_RESOURCES_READY;
            snapshot.preparedProcessorCount = snapshot.processorCount; snapshot.slatReady = 1;
            if (request.flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM) {
                snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_PREPARED;
            }
        } else if (request.command == KSWORD_ARK_HVM_CONTROL_SELF_TEST) {
            snapshot.stateFlags |= KSWORD_ARK_HVM_STATE_SELF_TEST_PASSED;
            snapshot.selfTestPassedProcessorCount = snapshot.processorCount;
        } else if (request.command == KSWORD_ARK_HVM_CONTROL_START_RESIDENT) {
            snapshot.stateFlags |= KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE;
            snapshot.residentProcessorCount = snapshot.processorCount;
            if (request.flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM) { snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_ARMED; }
            if (request.flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_VMX) { snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_VMX_ARMED; }
            if (request.flags & KSWORD_ARK_HVM_CONTROL_FLAG_HIDE_HYPERVISOR) { snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_HYPERVISOR_IDENTITY_HIDDEN; }
        } else if (request.command == KSWORD_ARK_HVM_CONTROL_STOP_RESIDENT) {
            snapshot.residentProcessorCount = 0; snapshot.stateFlags &= ~KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE;
        } else if (request.command == KSWORD_ARK_HVM_CONTROL_TEARDOWN) {
            snapshot.stateFlags &= ~KSWORD_ARK_HVM_STATE_RESOURCES_READY;
        }
        r.status = KSWORD_ARK_HVM_CONTROL_STATUS_OK;
        r.newGeneration = snapshot.generation; r.newStateFlags = snapshot.stateFlags;
    } else { io.ok = false; io.win32Error = ERROR_NOT_SUPPORTED; }
    return io;
}
}
// Only the unused watch page is stubbed. The control, guest and evidence pages
// below are the production widgets; this fixture cannot mutate real SCM state.
HvmWatchPanel::HvmWatchPanel(QWidget* parent) : QWidget(parent) {}
void HvmWatchPanel::refreshAsync() {}
void HvmWatchPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); }
namespace ks::settings { bool dangerousActionConfirmationsSuppressed() { return false; } }
namespace ks::ui {
bool confirmDestructiveAction(QWidget*, const QString&, const QString&, const QString&, const QString&) { return false; }
}
namespace ks::service {
bool QueryServiceStatus(const std::wstring&, ServiceStatus*, std::string*, std::uint32_t* error) {
    if (error) { *error = ERROR_SERVICE_DOES_NOT_EXIST; } return false;
}
bool StopServiceByName(const std::wstring&, std::uint32_t, std::uint32_t, ServiceStatus*, std::string*, std::uint32_t*) {
    ++scmMutations; return false;
}
bool StartServiceByName(const std::wstring&, std::uint32_t, std::uint32_t, ServiceStatus*, std::string*, std::uint32_t*) {
    ++scmMutations; return false;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    check(fontId >= 0, "offscreen Chinese/Latin font loaded");
    if (fontId >= 0) { app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first(), 9)); }
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("KSwordOfflineTests"));
    QCoreApplication::setApplicationName(QStringLiteral("HvmUi"));
    auto& language = ks::i18n::LanguageManager::instance();
    check(language.initialize(QStringLiteral("zh-CN")), "real language packs initialize");
    QFile protocol(QStringLiteral("shared/driver/KswordArkHvmIoctl.h"));
    check(protocol.open(QIODevice::ReadOnly), "protocol feature definitions readable");
    QRegularExpression featurePattern(QStringLiteral("#define\\s+KSWORD_ARK_HVM_FEATURE_[A-Z0-9_]+\\s+(0x[0-9A-Fa-f]+)ULL"));
    auto matches = featurePattern.globalMatch(QString::fromUtf8(protocol.readAll()));
    QSet<quint64> bits;
    while (matches.hasNext()) {
        const quint64 bit = matches.next().captured(1).toULongLong(nullptr, 16);
        check(bit && !(bit & (bit - 1)), "feature occupies one bit");
        check(!bits.contains(bit), "protocol feature bits are unique"); bits.insert(bit);
    }
    check(bits.contains(KSWORD_ARK_HVM_FEATURE_NESTED_SVM_ARMED) && bits.contains(KSWORD_ARK_HVM_FEATURE_NESTED_VMX_ARMED), "new activation bits audited");
    ksword::ark::DriverClient client;
    reset();
    check(!ksword::hvm::isNestedDispatchEnabled(), "nesting defaults off");
    check(ksword::hvm::queryState().residentAdmission, "native AMD admitted");
    snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_HYPERVISOR_PRESENT;
    std::memcpy(snapshot.hypervisorVendor, "VMwareVMware", 12);
    check(!ksword::hvm::queryState().residentAdmission, "VMware needs explicit opt-in");
    ksword::hvm::setNestedAllowed(true);
    check(ksword::hvm::queryState().residentAdmission, "opted-in VMware admitted");
    std::memcpy(snapshot.hypervisorVendor, "UnknownOuter", 12);
    check(!ksword::hvm::queryState().residentAdmission, "unknown outer rejected despite opt-in");
    reset(); snapshot.svmCapabilities.asidCount = 1;
    check(!ksword::hvm::queryState().residentAdmission, "ASID zero/one refused");
    reset(); snapshot.svmCapabilities.msrValidMask = 7;
    check(!ksword::hvm::queryState().residentAdmission, "incomplete privileged discovery refused");
    reset(); snapshot.featureFlags &= ~KSWORD_ARK_HVM_FEATURE_SVM_NRIP;
    check(!ksword::hvm::queryState().residentAdmission, "NRIP required");
    reset(); snapshot.queryStatus = KSWORD_ARK_HVM_QUERY_STATUS_FIRMWARE_DISABLED;
    check(ksword::hvm::queryState().availability == ksword::hvm::HvmAvailability::FirmwareDisabled, "firmware refusal preserved");
    reset(); snapshot.backend = KSWORD_ARK_HVM_BACKEND_NONE;
    check(!ksword::hvm::queryState().residentAdmission, "unknown backend closed");
    reset(); snapshot.featureFlags &= ~KSWORD_ARK_HVM_FEATURE_NESTED_SVM_DISPATCH;
    ksword::hvm::setNestedDispatchEnabled(true);
    check(!ksword::hvm::queryState().nestedSupported, "old AMD driver has no nesting capability");
    check(!ksword::hvm::startResident(1).ok && commands.empty(), "old AMD driver cannot prepare nesting");
    reset(); ksword::hvm::setNestedDispatchEnabled(true);
    ksword::hvm::setLocalEptEnabled(true); ksword::hvm::setEptpSwitchEnabled(true);
    ksword::hvm::setVeEnabled(true); ksword::hvm::setVmFuncEnabled(true); ksword::hvm::setHypervisorHidden(true);
    check(ksword::hvm::startResident(1).ok, "AMD ignores retained Intel preferences");
    check(commands.size() == 3, "prepare, self-test, start order");
    if (commands.size() == 3) {
        check(commands[0].expectedGeneration == 73 && commands[1].expectedGeneration == 74 && commands[2].expectedGeneration == 75, "fresh generation at each step");
        check((commands[0].flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM) &&
            (commands[2].flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM), "AMD nested flag at prepare and start");
        check(!(commands[1].flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM), "self-test keeps prepared mode");
        const unsigned long allowed = KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED | KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
            KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED | KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM;
        for (const auto& command : commands) { check((command.flags & ~allowed) == 0, "AMD wire contains no Intel flag"); }
    }
    check(ksword::hvm::queryState().nestedArmed, "activation comes from driver readback");
    reset(KSWORD_ARK_HVM_BACKEND_VMX); ksword::hvm::setNestedDispatchEnabled(true);
    check(ksword::hvm::startResident(1).ok, "Intel nesting still starts");
    check(commands.size() == 3 && (commands.back().flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_VMX) &&
        !(commands.back().flags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM), "Intel keeps VMX wire flag");
    reset(); ksword::hvm::setNestedDispatchEnabled(true); ksword::hvm::setWriteAccessEnabled(false);
    check(!ksword::hvm::startResident(1).ok && commands.empty(), "write permission enforced before prepare");
    for (unsigned long fail : {KSWORD_ARK_HVM_CONTROL_PREPARE, KSWORD_ARK_HVM_CONTROL_SELF_TEST, KSWORD_ARK_HVM_CONTROL_START_RESIDENT}) {
        reset(); failCommand = fail;
        check(!ksword::hvm::startResident(1).ok, "a failing step aborts start workflow");
        check(!commands.empty() && commands.back().command == fail, "no command follows failure");
    }
    reset(); bumpAtControl = true;
    check(!ksword::hvm::startResident(1).ok && commands.size() == 1, "generation race stops workflow");
    reset(); snapshot.stateFlags = KSWORD_ARK_HVM_STATE_RESOURCES_READY | KSWORD_ARK_HVM_STATE_SELF_TEST_PASSED;
    snapshot.preparedProcessorCount = 2; snapshot.selfTestPassedProcessorCount = 1;
    check(!ksword::hvm::queryState().selfTestPassed, "a passed bit cannot replace full-set test evidence");
    check(ksword::hvm::startResident(1).ok && commands.size() == 2 && commands.front().command == KSWORD_ARK_HVM_CONTROL_SELF_TEST, "incomplete test set is retested before start");
    reset(); snapshot.stateFlags |= KSWORD_ARK_HVM_STATE_RESOURCES_READY; ksword::hvm::setNestedDispatchEnabled(true);
    check(!ksword::hvm::startResident(1).ok && commands.empty(), "native preparation cannot be silently changed to nested");
    snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_PREPARED;
    ksword::hvm::setNestedDispatchEnabled(false);
    check(!ksword::hvm::startResident(1).ok && commands.empty(), "nested preparation cannot be silently changed to native");
    reset(); snapshot.residentProcessorCount = 1;
    snapshot.stateFlags = KSWORD_ARK_HVM_STATE_FAULTED | KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED;
    auto partial = ksword::hvm::queryState();
    check(partial.residentActive && !partial.residentComplete && !partial.residentAdmission, "partial rollback remains active but cannot acquire");
    check(ksword::hvm::stopResident(snapshot.generation).ok, "partial rollback can stop");
    reset();
    for (badQuery = 1; badQuery <= 4; ++badQuery) { check(!client.queryHvmStatus().io.ok, "malformed query rejected"); }
    badQuery = 0;
    for (badMetrics = 1; badMetrics <= 5; ++badMetrics) { check(!client.queryHvmMetrics().io.ok, "malformed metrics rejected"); }
    badMetrics = 0;
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("tools/hvm_lab/artifacts/ui-admission-20261001/screenshots");
    QDir().mkpath(output);
    for (const QString& lang : {QStringLiteral("zh-CN"), QStringLiteral("en-US")}) {
        check(language.setLanguage(lang), "language switch works");
        for (bool dark : {false, true}) {
            KswordTheme::SetDarkModeEnabled(dark);
            QPalette palette;
            palette.setColor(QPalette::Window, dark ? QColor(32,32,32) : QColor(245,245,245));
            palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
            palette.setColor(QPalette::Base, dark ? QColor(40,40,40) : Qt::white);
            palette.setColor(QPalette::Text, dark ? Qt::white : Qt::black);
            palette.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
            palette.setColor(QPalette::Button, dark ? QColor(55,55,55) : QColor(235,235,235));
            palette.setColor(QPalette::PlaceholderText, KswordTheme::TextSecondaryColor());
            palette.setColor(QPalette::AlternateBase, KswordTheme::SurfaceAltColor());
            palette.setColor(QPalette::Mid, KswordTheme::BorderColor());
            palette.setColor(QPalette::Disabled, QPalette::Text, KswordTheme::TextDisabledColor());
            palette.setColor(QPalette::Disabled, QPalette::ButtonText, KswordTheme::TextDisabledColor());
            app.setPalette(palette); app.setStyleSheet(ks::ui::BuildStatusRoleStyleRules());
            for (unsigned long backend : {KSWORD_ARK_HVM_BACKEND_VMX, KSWORD_ARK_HVM_BACKEND_SVM}) {
                reset(backend);
                HvmDock dock;
                dock.m_queryInFlight = true;
                dock.m_guestVmPanel->m_queryInFlight = true;
                dock.m_guestVmPanel->m_vmwareQueryValid = true;
                dock.m_hvmTab->m_firstRefreshStarted = true;
                dock.applyState(ksword::hvm::stateFromStatus(status()));
                dock.m_guestVmPanel->applyState(ksword::hvm::stateFromStatus(status()));
                dock.m_hvmTab->applyStatus(status());
                if (lang == QStringLiteral("en-US")) {
                    check(!QRegularExpression(QStringLiteral("[\\x{4e00}-\\x{9fff}]"))
                        .match(dock.m_hvmTab->m_summaryLabel->text()).hasMatch(), "English evidence summary contains no untranslated Chinese");
                }
                const bool amd = backend == KSWORD_ARK_HVM_BACKEND_SVM;
                check(dock.m_hookWizardButton->isHidden() == amd && dock.m_soakButton->isHidden() == amd, "Intel-only dock actions adapted");
                check(dock.m_selfTestButton->isHidden() != amd, "AMD all-CPU test step adapted");
                check(dock.m_guestVmPanel->m_stepHideIdentity.container->isHidden() == amd, "AMD omits identity card");
                for (int width : {520, 1050}) {
                    dock.m_tabs->setCurrentIndex(1);
                    dock.resize(width, 800); dock.show(); dock.m_pollTimer->stop(); app.processEvents();
                    check(dock.width() == width, "dock respects narrow viewport");
                    const QString stem = QStringLiteral("%1-%2-%3-%4").arg(lang).arg(dark ? "dark" : "light").arg(amd ? "amd" : "intel").arg(width);
                    check(dock.grab().save(output + QLatin1Char('/') + stem + QStringLiteral("-control.png")), "control screenshot saved");
                    dock.m_tabs->setCurrentWidget(dock.m_guestVmPanel); app.processEvents();
                    check(dock.grab().save(output + QLatin1Char('/') + stem + QStringLiteral("-guest.png")), "guest screenshot saved");
                    dock.m_tabs->setCurrentWidget(dock.m_hvmTab);
                    if (amd) { dock.m_hvmTab->applyMetrics(client.queryHvmMetrics()); }
                    app.processEvents();
                    check(dock.grab().save(output + QLatin1Char('/') + stem + QStringLiteral("-evidence.png")), "evidence screenshot saved");
                    dock.hide();
                }
                if (amd) {
                    check(dock.m_hvmTab->m_cpuTable->item(0,2)->text() == ks::i18n::sourceText(QStringLiteral("暂不可用")), "invalid raw exit is unavailable, not zero");
                    auto& raw = metrics->svmProcessors[0];
                    raw.valid = 1; raw.sequence = 2; raw.exitCode = 0xFEDCBA9876543210ULL;
                    raw.general.valid = 1; raw.general.sequence = 2; raw.general.hardwareExits = 0x100000002ULL;
                    dock.m_hvmTab->applyMetrics(client.queryHvmMetrics());
                    check(dock.m_hvmTab->m_cpuTable->item(0,2)->text().contains(QStringLiteral("fedcba9876543210")), "AMD raw exit keeps all 64 bits");
                    check(dock.m_hvmTab->m_cpuTable->item(0,7)->text() == QStringLiteral("4294967298"), "L2 count keeps all 64 bits");
                    raw.general.sequence = 3; dock.m_hvmTab->applyMetrics(client.queryHvmMetrics());
                    check(dock.m_hvmTab->m_cpuTable->item(0,7)->text() == ks::i18n::sourceText(QStringLiteral("暂不可用")), "odd general record is independently unavailable");
                    metrics->svmProcessors[0].generation++;
                    dock.m_hvmTab->applyStatus(status()); dock.m_hvmTab->applyMetrics(client.queryHvmMetrics());
                    check(dock.m_hvmTab->m_cpuTable->columnCount() == 6, "different generation not merged");
                    snapshot.stateFlags = KSWORD_ARK_HVM_STATE_FAULTED | KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED;
                    snapshot.residentProcessorCount = 1;
                    dock.m_hvmTab->applyStatus(status()); dock.applyState(ksword::hvm::stateFromStatus(status()));
                    check(dock.m_hvmTab->m_stopResidentButton->isEnabled() && dock.m_residentButton->isEnabled(), "both pages can stop partial rollback");
                    for (int scenario = 0; scenario < 6; ++scenario) {
                        reset(); bool admitted = false;
                        if (scenario == 0) { snapshot.backend = KSWORD_ARK_HVM_BACKEND_NONE; }
                        if (scenario == 1) { snapshot.queryStatus = KSWORD_ARK_HVM_QUERY_STATUS_FIRMWARE_DISABLED; }
                        if (scenario >= 2 && scenario <= 4) {
                            snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_HYPERVISOR_PRESENT;
                            std::memcpy(snapshot.hypervisorVendor, scenario == 4 ? "UnknownOuter" : "VMwareVMware", 12);
                            ksword::hvm::setNestedAllowed(scenario != 2); admitted = scenario == 3;
                        }
                        if (scenario == 5) { snapshot.featureFlags &= ~KSWORD_ARK_HVM_FEATURE_NESTED_SVM_DISPATCH; admitted = true; }
                        const auto state = ksword::hvm::stateFromStatus(status());
                        dock.applyState(state); dock.m_guestVmPanel->applyState(state); dock.m_hvmTab->applyStatus(status());
                        check(dock.m_prepareButton->isEnabled() == admitted && dock.m_hvmTab->m_prepareButton->isEnabled() == admitted, "control and evidence use identical environment gate");
                        check(dock.m_guestVmPanel->m_doAll->isEnabled() == (admitted && scenario != 5), "guest activation observes hardware and old-driver gate");
                        dock.m_tabs->setCurrentWidget(dock.m_guestVmPanel); dock.resize(520,800); dock.show(); dock.m_pollTimer->stop(); app.processEvents();
                        check(dock.grab().save(output + QStringLiteral("/%1-%2-amd-env-%3.png").arg(lang).arg(dark ? "dark" : "light").arg(scenario)), "environment screenshot saved"); dock.hide();
                    }
                }
            }
        }
    }
    reset();
    {
        HvmDock dock; dock.m_queryInFlight = true;
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, []() {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) { dialog->accept(); }
        });
        dismiss.start(10);
        dock.m_hvmTab->runControlAsync(KSWORD_ARK_HVM_CONTROL_PREPARE, false);
        QElapsedTimer deadline; deadline.start();
        while (dock.m_hvmTab->m_operationRunning && deadline.elapsed() < 5000) { app.processEvents(); }
        check(!dock.m_hvmTab->m_operationRunning && dock.m_hvmTab->m_cpuTable->columnCount() == 9,
            "AMD evidence includes fresh metrics after lifecycle control");
    }
    reset(); HvmGuestVmPanel panel; panel.m_queryInFlight = true;
    failCommand = KSWORD_ARK_HVM_CONTROL_START_RESIDENT;
    HvmGuestVmPanel::enableAllSwitches();
    check(!HvmGuestVmPanel::startMonitor().isEmpty(), "guest workflow reports start failure");
    check(scmMutations == 0, "failure never restarts VMware services");
    reset(); ksword::hvm::setNestedDispatchEnabled(true); snapshot.stateFlags = KSWORD_ARK_HVM_STATE_RESOURCES_READY | KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE;
    snapshot.residentProcessorCount = 2;
    snapshot.featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_PREPARED | KSWORD_ARK_HVM_FEATURE_NESTED_SVM_ARMED;
    panel.m_vmwareQueryValid = false; panel.applyState(ksword::hvm::stateFromStatus(status()));
    check(panel.m_stepRestartVmware.status->text() != ks::i18n::sourceText(QStringLiteral("没装 VMware")), "SCM query failure is not treated as VMware absent");
    check(panel.m_verdict->text() == ks::i18n::sourceText(QStringLiteral("无法查询 VMware 驱动服务，未继续重启。")), "SCM failure cannot complete workflow");
    snapshot.backend = KSWORD_ARK_HVM_BACKEND_NONE; panel.applyState(ksword::hvm::stateFromStatus(status()));
    check(!panel.m_doAll->isEnabled() && !panel.m_stepStartMonitor.action->isEnabled(), "unknown backend closes guest actions");
    std::cout << "HVM_UI_TESTS checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
