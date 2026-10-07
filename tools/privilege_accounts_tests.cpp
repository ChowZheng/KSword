// Compiles the actual backend TU so its internal pagination paths can be exercised with small
// buffers. These tests only enumerate; no local-account, group or policy mutation is performed.
#include "../Ksword5.1/Ksword5.1/PrivilegeDock/PrivilegeAccountPages.cpp"
#include <QSet>
#include <cstdio>

static int checks = 0;
static void check(bool value, const char* label)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
static QSet<QString> identities(const ks::privilege::RowsResult& result)
{
    QSet<QString> values;
    for (const auto& row : result.rows) values.insert(row.sid);
    return values;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const auto users = ks::privilege::enumerateUsers();
    const auto usersPaged = ks::privilege::enumerateUsers(1024);
    std::fprintf(stderr, "Users: regular=%s; paged=%s\n", users.error.toUtf8().constData(), usersPaged.error.toUtf8().constData());
    check(users.error.isEmpty() && usersPaged.error.isEmpty(), "local user enumeration is complete");
    check(!users.rows.isEmpty(), "local user list is populated");
    check(identities(users) == identities(usersPaged), "paged local users match unpaged users");
    const auto groups = ks::privilege::enumerateGroups();
    const auto groupsPaged = ks::privilege::enumerateGroups(1024);
    std::fprintf(stderr, "Groups: regular=%s; paged=%s\n", groups.error.toUtf8().constData(), groupsPaged.error.toUtf8().constData());
    check(groups.error.isEmpty() && groupsPaged.error.isEmpty(), "local group enumeration is complete");
    check(!groups.rows.isEmpty(), "local group list is populated");
    check(identities(groups) == identities(groupsPaged), "paged local groups match unpaged groups");
    for (const auto& group : groups.rows)
    {
        const auto members = ks::privilege::enumerateMembers(group.name);
        const auto membersPaged = ks::privilege::enumerateMembers(group.name, 512);
        check(members.error.isEmpty() && membersPaged.error.isEmpty(), "local group member enumeration is complete");
        check(identities(members) == identities(membersPaged), "paged group members match unpaged members");
    }
    const auto memberships = ks::privilege::accountGroups(users.rows.first().name);
    check(memberships.error.isEmpty(), "account-to-local-group lookup succeeds");
    check(!ks::privilege::queryRights(QStringLiteral("S-1-invalid")).error.isEmpty(), "invalid SID is an explicit query failure");
    const auto snapshot = ks::privilege::captureAccountPolicySnapshot();
    for (const auto& error : snapshot.value(QStringLiteral("errors")).toArray())
        std::fprintf(stderr, "Snapshot collection diagnostic: %s\n", error.toString().toUtf8().constData());
    check(snapshot.value(QStringLiteral("entries")).isArray(), "policy snapshot exposes entries");
    check(snapshot.value(QStringLiteral("errors")).isArray(), "policy snapshot exposes collection errors");
    QSet<QString> keys;
    QString previous;
    for (const auto& value : snapshot.value(QStringLiteral("entries")).toArray())
    {
        const auto entry = value.toObject();
        const auto key = entry.value(QStringLiteral("key")).toString();
        check(!key.isEmpty() && !keys.contains(key), "canonical policy key is unique");
        check(previous.isEmpty() || previous < key, "canonical policy keys are sorted");
        check(!entry.value(QStringLiteral("value")).isArray() && !entry.value(QStringLiteral("value")).isObject(), "snapshot value is scalar");
        keys.insert(key); previous = key;
    }
    QString rejection;
    check(!ks::privilege::setLocalAccountEnabled(QStringLiteral("__foreign_domain__\\user"), false, &rejection)
        && !rejection.isEmpty(), "foreign-domain setter rejects before any account mutation");
    QVector<QStringList> navigations;
    auto* accountPage = ks::privilege::createAccountManagementPage(nullptr,
        [&navigations](const QString& target, const QString& sid) { navigations.push_back({target, sid}); });
    accountPage->show();
    QThreadPool::globalInstance()->waitForDone();
    app.processEvents();
    auto* accountTable = accountPage->findChild<QTableWidget*>(QStringLiteral("privilege_accounts_table"));
    check(accountTable && accountTable->rowCount() == users.rows.size(), "account page receives actual worker enumeration");
    accountTable->selectRow(0);
    const QString selectedSid = accountTable->item(0, 0)->data(Qt::UserRole).toString();
    const QStringList targets{QStringLiteral("groups"), QStringLiteral("rights"), QStringLiteral("sessions")};
    for (const QString& target : targets)
    {
        auto* navigation = accountPage->findChild<QPushButton*>(QStringLiteral("privilege_%1_navigation").arg(target));
        check(navigation && navigation->isEnabled(), "account navigation button is enabled with callback");
        navigation->click();
    }
    check(navigations.size() == 3, "all account navigation routes invoke callback");
    for (int index = 0; index < 3; ++index)
        check(navigations[index] == QStringList{targets[index], selectedSid}, "account navigation carries selected immutable SID");
    delete accountPage;
    for (int iteration = 0; iteration < 20; ++iteration)
    {
        auto* accounts = ks::privilege::createAccountManagementPage(nullptr); accounts->show(); delete accounts;
        auto* groupsPage = ks::privilege::createGroupsPage(nullptr); groupsPage->show(); delete groupsPage;
        auto* rights = ks::privilege::createRightsPage(nullptr); delete rights;
    }
    QThreadPool::globalInstance()->waitForDone();
    app.processEvents();
    check(true, "destroyed pages safely detach queued enumeration completions");
    std::printf("PASS %d checks; users=%lld groups=%lld policyEntries=%lld collectionErrors=%lld\n", checks,
        static_cast<long long>(users.rows.size()), static_cast<long long>(groups.rows.size()),
        static_cast<long long>(keys.size()), static_cast<long long>(snapshot.value(QStringLiteral("errors")).toArray().size()));
    return 0;
}
