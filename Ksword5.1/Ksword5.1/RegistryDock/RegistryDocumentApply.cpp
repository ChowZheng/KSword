#include "RegistryDocumentApply.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
constexpr qsizetype kOperationLimit = 300000;
constexpr qint64 kJournalLimit = 192LL * 1024 * 1024;
constexpr qint64 kPlanDataLimit = 128LL * 1024 * 1024;

bool failure(QString& error, const QString& message) { error = message; return false; }
QString folded(const QString& text) { return text.toCaseFolded(); }
bool inside(const QString& path, const QString& root)
{
    return path.compare(root, Qt::CaseInsensitive) == 0
        || path.startsWith(root + QLatin1Char('\\'), Qt::CaseInsensitive);
}

bool pathCanonical(const QString& source, QString& path, QString& error)
{
    const QString original = source;
    const QStringList roots { QStringLiteral("HKEY_CLASSES_ROOT"), QStringLiteral("HKEY_CURRENT_USER"),
        QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("HKEY_USERS"), QStringLiteral("HKEY_CURRENT_CONFIG") };
    const QStringList aliases { QStringLiteral("HKCR"), QStringLiteral("HKCU"), QStringLiteral("HKLM"),
        QStringLiteral("HKU"), QStringLiteral("HKCC") };
    if (source.isEmpty() || source.size() > 32767 || !source.isValidUtf16()
        || source.contains(QChar(0)) || source.endsWith(QLatin1Char('\\')))
        return failure(error, QStringLiteral("Invalid registry change path."));
    const qsizetype slash = original.indexOf(QLatin1Char('\\'));
    const QString root = slash < 0 ? original : original.left(slash);
    int rootIndex = -1;
    for (int i = 0; i < roots.size(); ++i) {
        if (root.compare(roots.at(i), Qt::CaseInsensitive) == 0
            || root.compare(aliases.at(i), Qt::CaseInsensitive) == 0)
            rootIndex = i;
    }
    if (rootIndex < 0)
        return failure(error, QStringLiteral("Unsupported registry change root."));
    path = roots.at(rootIndex);
    if (slash >= 0) {
        const QStringList components = original.mid(slash + 1).split(QLatin1Char('\\'));
        if (components.size() > 256)
            return failure(error, QStringLiteral("Registry change path exceeds the depth limit."));
        for (const QString& component : components) {
            if (component.isEmpty() || component.size() > 255)
                return failure(error, QStringLiteral("Invalid registry change key component."));
        }
        path += QLatin1Char('\\') + original.mid(slash + 1);
    }
    return true;
}

QString parentPath(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('\\'));
    return slash < 0 ? QString() : path.left(slash);
}

bool sameValue(const RegistryApplyValueState& a, const RegistryApplyValueState& b)
{
    return a.exists == b.exists && (!a.exists || (a.type == b.type && a.data == b.data));
}

bool hasLink(const RegistryDocument& tree)
{
    for (const auto& value : tree.values) {
        if (value.type == 6 && value.name.compare(QStringLiteral("SymbolicLinkValue"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool sameTree(const RegistryDocument& expected, const RegistryDocument& actual)
{
    if (expected.keys.size() != actual.keys.size() || expected.values.size() != actual.values.size())
        return false;
    QHash<QString, QByteArray> actualKeys;
    for (const auto& key : actual.keys)
        actualKeys.insert(folded(key.path), key.securityDescriptor);
    for (const auto& key : expected.keys) {
        const auto found = actualKeys.constFind(folded(key.path));
        if (found == actualKeys.constEnd() || (!key.securityDescriptor.isEmpty() && key.securityDescriptor != found.value()))
            return false;
    }
    QHash<QString, QHash<QString, RegistryApplyValueState>> values;
    for (const auto& value : actual.values)
        values[folded(value.keyPath)].insert(folded(value.name), {true, value.type, value.data});
    for (const auto& value : expected.values) {
        const auto path = values.constFind(folded(value.keyPath));
        if (path == values.constEnd())
            return false;
        const auto found = path.value().constFind(folded(value.name));
        if (found == path.value().constEnd() || !sameValue({true, value.type, value.data}, found.value()))
            return false;
    }
    return true;
}

struct SimulatedKey
{
    QString path;
    QByteArray security;
    QHash<QString, RegistryApplyValueState> values;
    QHash<QString, QString> names;
};

struct Preparation
{
    RegistryApplyBackend* backend = nullptr;
    RegistryApplyPlan plan;
    QHash<QString, SimulatedKey> keys;
    QStringList absentPrefixes;
    QString* error = nullptr;

    bool exists(const QString& path, bool& present)
    {
        if (keys.contains(folded(path))) { present = true; return true; }
        for (const QString& missing : absentPrefixes) {
            if (inside(path, missing)) { present = false; return true; }
        }
        if (!backend->keyExists(path, present, *error))
            return false;
        if (present)
            keys.insert(folded(path), {path, {}, {}, {}});
        else
            absentPrefixes.append(path);
        return true;
    }

    bool ensure(const QString& path, bool explicitSection)
    {
        bool present = false;
        if (!exists(path, present))
            return false;
        if (!present) {
            const QString parent = parentPath(path);
            if (parent.isEmpty())
                return failure(*error, QStringLiteral("A predefined registry root is unavailable."));
            if (!ensure(parent, false))
                return false;
        }
        if (!present || explicitSection) {
            RegistryApplyOperation op;
            op.kind = RegistryApplyOperation::Kind::CreateKey;
            op.keyPath = path;
            op.keyExistedBefore = present;
            plan.operations.append(std::move(op));
        }
        if (!present)
            keys.insert(folded(path), {path, {}, {}, {}});
        return true;
    }

    RegistryDocument subtree(const QString& root) const
    {
        RegistryDocument tree;
        tree.rootPath = root;
        tree.viewBits = plan.viewBits;
        QStringList paths;
        for (const auto& key : keys) {
            if (inside(key.path, root))
                paths.append(key.path);
        }
        std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
        for (const auto& path : paths) {
            const auto& key = keys[folded(path)];
            tree.keys.append({key.path, false, key.security});
            for (auto it = key.values.constBegin(); it != key.values.constEnd(); ++it) {
                if (it.value().exists)
                    tree.values.append({key.path, key.names.value(it.key()), it.value().type, it.value().data, false});
            }
        }
        return tree;
    }
};

QVector<RegistryDocumentOperation> sourceOrder(const RegistryDocument& document)
{
    if (!document.operationOrder.isEmpty())
        return document.operationOrder;
    QVector<RegistryDocumentOperation> order;
    for (qsizetype i = 0; i < document.keys.size(); ++i)
        order.append({RegistryDocumentOperation::Kind::Key, i});
    for (qsizetype i = 0; i < document.values.size(); ++i)
        order.append({RegistryDocumentOperation::Kind::Value, i});
    return order;
}
} // namespace

bool RegistryApplyBackend::captureTreeBounded(const QString& path, qint64 maximumDataBytes,
    RegistryDocument& tree, QString& error)
{
    if (!captureTree(path, tree, error))
        return false;
    qint64 bytes = 0;
    for (const auto& key : tree.keys)
        bytes += key.securityDescriptor.size();
    for (const auto& value : tree.values)
        bytes += value.data.size();
    if (bytes > maximumDataBytes) {
        tree = {};
        return failure(error, QStringLiteral("Registry original capture exceeds its remaining data budget."));
    }
    return true;
}

bool RegistryDocumentApplyService::prepareWithBackend(const RegistryDocument& document,
    RegistryApplyBackend& backend, RegistryApplyPlan& plan, QString& error)
{
    plan = {};
    error.clear();
    if ((document.viewBits != 0 && document.viewBits != 32 && document.viewBits != 64)
        || document.keys.size() + document.values.size() > kOperationLimit)
        return failure(error, QStringLiteral("Registry change view or operation count is invalid."));
    RegistryDocument source = document;
    QStringList roots;
    qint64 dataBytes = 0;
    for (auto& key : source.keys) {
        if (!pathCanonical(key.path, key.path, error))
            return false;
        if (key.deleteTree && parentPath(key.path).isEmpty())
            return failure(error, QStringLiteral("Deleting a predefined registry root is forbidden."));
        roots.append(key.path);
    }
    for (auto& value : source.values) {
        if (!pathCanonical(value.keyPath, value.keyPath, error))
            return false;
        if (value.name.size() > 16383 || !value.name.isValidUtf16() || value.name.contains(QChar(0))
            || value.data.size() > 16 * 1024 * 1024 || (value.deleteValue && !value.data.isEmpty()))
            return failure(error, QStringLiteral("Registry change value name or data is invalid."));
        dataBytes += value.data.size();
        if (dataBytes > kPlanDataLimit)
            return failure(error, QStringLiteral("Registry change exceeds the plan data budget."));
        roots.append(value.keyPath);
    }
    const auto order = sourceOrder(source);
    if (order.isEmpty() || order.size() != source.keys.size() + source.values.size())
        return failure(error, QStringLiteral("Registry change operation order is incomplete."));
    QSet<qsizetype> keyIndices, valueIndices;
    for (const auto& entry : order) {
        if (entry.kind == RegistryDocumentOperation::Kind::Key) {
            if (entry.index < 0 || entry.index >= source.keys.size() || keyIndices.contains(entry.index))
                return failure(error, QStringLiteral("Invalid registry change key operation index."));
            keyIndices.insert(entry.index);
        } else if (entry.kind == RegistryDocumentOperation::Kind::Value) {
            if (entry.index < 0 || entry.index >= source.values.size() || valueIndices.contains(entry.index))
                return failure(error, QStringLiteral("Invalid registry change value operation index."));
            valueIndices.insert(entry.index);
        } else {
            return failure(error, QStringLiteral("Unknown registry change operation kind."));
        }
    }
    std::sort(roots.begin(), roots.end(), [](const QString& a, const QString& b) {
        return (a + QLatin1Char('\\')).compare(b + QLatin1Char('\\'), Qt::CaseInsensitive) < 0;
    });
    QStringList selectedRoots;
    for (const auto& root : roots) {
        if (selectedRoots.isEmpty() || !inside(root, selectedRoots.last()))
            selectedRoots.append(root);
    }
    if (selectedRoots.size() > 100000)
        return failure(error, QStringLiteral("Registry change exceeds the target subtree count limit."));
    Preparation state;
    state.backend = &backend;
    state.error = &error;
    state.plan.viewBits = source.viewBits;
    qsizetype totalKeys = 0, totalValues = 0;
    for (const auto& root : selectedRoots) {
        bool present = false;
        if (!backend.keyExists(root, present, error))
            return false;
        if (!present) {
            state.absentPrefixes.append(root);
            state.plan.originallyAbsentKeys.append(root);
            continue;
        }
        RegistryDocument original;
        if (!backend.captureTreeBounded(root, kPlanDataLimit - dataBytes, original, error))
            return false;
        if (original.rootPath.compare(root, Qt::CaseInsensitive) != 0 || original.keys.isEmpty())
            return failure(error, QStringLiteral("Registry backend returned an incomplete original subtree."));
        totalKeys += original.keys.size();
        totalValues += original.values.size();
        if (totalKeys > 100000 || totalValues > 250000)
            return failure(error, QStringLiteral("Registry originals exceed the key or value budget."));
        for (const auto& key : original.keys) {
            if (!inside(key.path, root) || key.deleteTree)
                return failure(error, QStringLiteral("Registry backend returned an invalid original key."));
            state.keys.insert(folded(key.path), {key.path, key.securityDescriptor, {}, {}});
            dataBytes += key.securityDescriptor.size();
        }
        for (const auto& value : original.values) {
            auto found = state.keys.find(folded(value.keyPath));
            if (found == state.keys.end() || value.deleteValue)
                return failure(error, QStringLiteral("Registry backend returned an invalid original value."));
            found.value().values.insert(folded(value.name), {true, value.type, value.data});
            found.value().names.insert(folded(value.name), value.name);
            dataBytes += value.data.size();
        }
        if (dataBytes > kPlanDataLimit)
            return failure(error, QStringLiteral("Registry originals exceed the plan data budget."));
        state.plan.originalSubtrees.append(std::move(original));
    }
    for (const auto& entry : order) {
        if (entry.kind == RegistryDocumentOperation::Kind::Key) {
            const auto& key = source.keys.at(entry.index);
            if (!key.deleteTree) {
                if (!state.ensure(key.path, true))
                    return false;
            } else {
                RegistryApplyOperation op;
                op.kind = RegistryApplyOperation::Kind::DeleteTree;
                op.keyPath = key.path;
                if (!state.exists(key.path, op.keyExistedBefore))
                    return false;
                if (op.keyExistedBefore) {
                    op.beforeTree = state.subtree(key.path);
                    if (hasLink(op.beforeTree))
                        return failure(error, QStringLiteral("Registry tree deletion refuses symbolic links."));
                    const auto paths = state.keys.keys();
                    for (const auto& path : paths) {
                        if (inside(state.keys.value(path).path, key.path))
                            state.keys.remove(path);
                    }
                }
                state.absentPrefixes.append(key.path);
                state.plan.operations.append(std::move(op));
            }
        } else {
            const auto& value = source.values.at(entry.index);
            if (!state.ensure(value.keyPath, false))
                return false;
            auto& key = state.keys[folded(value.keyPath)];
            const auto link = key.values.constFind(folded(QStringLiteral("SymbolicLinkValue")));
            if (link != key.values.constEnd() && link.value().exists && link.value().type == 6)
                return failure(error, QStringLiteral("Registry value editing refuses symbolic links."));
            RegistryApplyOperation op;
            op.kind = value.deleteValue ? RegistryApplyOperation::Kind::DeleteValue : RegistryApplyOperation::Kind::SetValue;
            op.keyPath = value.keyPath;
            op.valueName = value.name;
            op.keyExistedBefore = true;
            op.beforeValue = key.values.value(folded(value.name));
            op.afterValue = value.deleteValue ? RegistryApplyValueState{} : RegistryApplyValueState{true, value.type, value.data};
            key.values.insert(folded(value.name), op.afterValue);
            key.names.insert(folded(value.name), value.name);
            state.plan.operations.append(std::move(op));
        }
        if (state.plan.operations.size() > kOperationLimit)
            return failure(error, QStringLiteral("Registry change exceeds the expanded operation budget."));
    }
    plan = std::move(state.plan);
    return true;
}

bool RegistryDocumentApplyService::applyWithBackend(const RegistryApplyPlan& plan,
    RegistryApplyBackend& backend, RegistryApplyResult& result, const std::atomic_bool* canceledToken)
{
    result = {};
    result.viewBits = plan.viewBits;
    if ((plan.viewBits != 0 && plan.viewBits != 32 && plan.viewBits != 64)
        || plan.operations.isEmpty() || plan.operations.size() > kOperationLimit)
        return failure(result.error, QStringLiteral("Invalid registry apply plan."));
    for (const auto& operation : plan.operations) {
        QString normalized;
        if (!pathCanonical(operation.keyPath, normalized, result.error)
            || normalized != operation.keyPath
            || (operation.kind == RegistryApplyOperation::Kind::DeleteTree && parentPath(normalized).isEmpty())
            || operation.valueName.size() > 16383 || !operation.valueName.isValidUtf16()
            || operation.valueName.contains(QChar(0))
            || operation.beforeValue.data.size() > 16 * 1024 * 1024
            || operation.afterValue.data.size() > 16 * 1024 * 1024)
            return failure(result.error, QStringLiteral("Registry apply plan contains an unsafe path or value."));
        if ((operation.kind == RegistryApplyOperation::Kind::SetValue && !operation.afterValue.exists)
            || (operation.kind == RegistryApplyOperation::Kind::DeleteValue && operation.afterValue.exists))
            return failure(result.error, QStringLiteral("Registry apply plan contains inconsistent value state."));
        if (operation.kind == RegistryApplyOperation::Kind::DeleteTree && operation.keyExistedBefore
            && (operation.beforeTree.rootPath.compare(normalized, Qt::CaseInsensitive) != 0
                || operation.beforeTree.keys.isEmpty() || hasLink(operation.beforeTree)))
            return failure(result.error, QStringLiteral("Registry apply plan has no valid original deletion subtree."));
        RegistryApplyReceipt receipt;
        receipt.operation = operation;
        result.receipts.append(std::move(receipt));
    }
    bool mutated = false, treeDeletion = false;
    for (auto& receipt : result.receipts) {
        auto stopForCancellation = [&]() {
            if (!canceledToken || !canceledToken->load(std::memory_order_relaxed))
                return false;
            receipt.state = RegistryApplyReceipt::State::Canceled;
            result.canceled = true;
            result.error = QStringLiteral("Registry application was canceled; completed changes remain applied.");
            result.canUndo = mutated && !treeDeletion;
            return true;
        };
        if (stopForCancellation())
            return false;
        auto& op = receipt.operation;
        QString error;
        bool ok = true;
        if (op.kind == RegistryApplyOperation::Kind::CreateKey || op.kind == RegistryApplyOperation::Kind::DeleteTree) {
            bool present = false;
            ok = backend.keyExists(op.keyPath, present, error);
            if (ok && stopForCancellation())
                return false;
            if (ok && present != op.keyExistedBefore)
                ok = failure(error, QStringLiteral("Registry key changed after preview: %1").arg(op.keyPath));
            if (ok && op.kind == RegistryApplyOperation::Kind::CreateKey && !present) {
                if (stopForCancellation())
                    return false;
                receipt.mutated = true;
                ok = backend.createKey(op.keyPath, error);
                if (ok) {
                    bool after = false;
                    ok = backend.keyExists(op.keyPath, after, error);
                    if (ok && !after)
                        ok = failure(error, QStringLiteral("Created registry key could not be verified."));
                    if (ok)
                        ok = backend.captureTree(op.keyPath, receipt.actualAfterTree, error);
                    if (ok && (receipt.actualAfterTree.keys.size() != 1 || !receipt.actualAfterTree.values.isEmpty()))
                        ok = failure(error, QStringLiteral("Created registry key changed during verification."));
                }
            } else if (ok && op.kind == RegistryApplyOperation::Kind::DeleteTree && present) {
                RegistryDocument current;
                ok = backend.captureTree(op.keyPath, current, error);
                if (ok && stopForCancellation())
                    return false;
                if (ok && (hasLink(current) || !sameTree(op.beforeTree, current)))
                    ok = failure(error, QStringLiteral("Registry subtree changed after preview: %1").arg(op.keyPath));
                if (ok) {
                    if (stopForCancellation())
                        return false;
                    receipt.mutated = true;
                    treeDeletion = true;
                    ok = backend.deleteTree(op.keyPath, op.beforeTree, error);
                    bool after = true;
                    if (ok)
                        ok = backend.keyExists(op.keyPath, after, error);
                    if (ok && after)
                        ok = failure(error, QStringLiteral("Deleted registry subtree is still present."));
                }
            }
        } else if (op.kind == RegistryApplyOperation::Kind::SetValue || op.kind == RegistryApplyOperation::Kind::DeleteValue) {
            RegistryApplyValueState before;
            ok = backend.readValue(op.keyPath, op.valueName, before, error);
            if (ok && stopForCancellation())
                return false;
            if (ok && !sameValue(before, op.beforeValue))
                ok = failure(error, QStringLiteral("Registry value changed after preview: %1\\%2").arg(op.keyPath, op.valueName));
            if (ok && !sameValue(before, op.afterValue)) {
                if (stopForCancellation())
                    return false;
                receipt.mutated = true;
                ok = op.afterValue.exists
                    ? backend.setValue(op.keyPath, op.valueName, op.afterValue.type, op.afterValue.data, error)
                    : backend.deleteValue(op.keyPath, op.valueName, error);
            }
            if (ok)
                ok = backend.readValue(op.keyPath, op.valueName, receipt.actualAfterValue, error);
            if (ok && !sameValue(receipt.actualAfterValue, op.afterValue))
                ok = failure(error, QStringLiteral("Registry write verification did not match the requested raw data."));
        } else {
            ok = failure(error, QStringLiteral("Unknown registry apply operation."));
        }
        if (!ok) {
            receipt.state = RegistryApplyReceipt::State::Failed;
            receipt.error = error;
            result.error = error;
            result.canceled = canceledToken && canceledToken->load(std::memory_order_relaxed);
            // A failed mutating call may have partially changed the target. It
            // cannot be automatically undone without a proven post-write state.
            result.canUndo = mutated && !treeDeletion && !receipt.mutated;
            return false;
        }
        receipt.state = RegistryApplyReceipt::State::Success;
        mutated = mutated || receipt.mutated;
    }
    result.completed = true;
    result.canUndo = mutated && !treeDeletion;
    return true;
}

bool RegistryDocumentApplyService::undoWithBackend(const RegistryApplyResult& previous,
    RegistryApplyBackend& backend, RegistryApplyResult& result, const std::atomic_bool* canceledToken)
{
    result = {};
    if (!previous.canUndo)
        return failure(result.error, QStringLiteral("This committed change requires restoration from its original backup."));
    RegistryApplyPlan undo;
    undo.viewBits = previous.viewBits;
    const auto& originals = previous.undoAttempt ? previous.pendingUndoReceipts : previous.receipts;
    QVector<qsizetype> originalIndices;
    for (qsizetype i = originals.size(); i > 0; --i) {
        const auto& receipt = originals.at(i - 1);
        if (receipt.state != RegistryApplyReceipt::State::Success || !receipt.mutated)
            continue;
        RegistryApplyOperation op;
        op.keyPath = receipt.operation.keyPath;
        op.valueName = receipt.operation.valueName;
        op.keyExistedBefore = true;
        if (receipt.operation.kind == RegistryApplyOperation::Kind::CreateKey) {
            op.kind = RegistryApplyOperation::Kind::DeleteTree;
            op.beforeTree = receipt.actualAfterTree;
        } else if (receipt.operation.kind == RegistryApplyOperation::Kind::SetValue
            || receipt.operation.kind == RegistryApplyOperation::Kind::DeleteValue) {
            op.kind = receipt.operation.beforeValue.exists
                ? RegistryApplyOperation::Kind::SetValue : RegistryApplyOperation::Kind::DeleteValue;
            op.beforeValue = receipt.actualAfterValue;
            op.afterValue = receipt.operation.beforeValue;
        } else {
            return failure(result.error, QStringLiteral("Automatic undo refuses committed subtree deletions."));
        }
        undo.operations.append(std::move(op));
        originalIndices.append(i - 1);
    }
    if (undo.operations.isEmpty())
        return failure(result.error, QStringLiteral("There are no completed registry changes to undo."));
    const bool ok = applyWithBackend(undo, backend, result, canceledToken);
    result.undoAttempt = true;
    bool uncertainMutation = result.receipts.size() != originalIndices.size();
    for (qsizetype i = originalIndices.size(); i > 0; --i) {
        const qsizetype inverseIndex = i - 1;
        if (inverseIndex < result.receipts.size()) {
            const auto& inverse = result.receipts.at(inverseIndex);
            if (inverse.state == RegistryApplyReceipt::State::Success)
                continue;
            uncertainMutation = uncertainMutation || inverse.mutated;
        }
        result.pendingUndoReceipts.append(originals.at(originalIndices.at(inverseIndex)));
    }
    result.canUndo = !uncertainMutation && !result.pendingUndoReceipts.isEmpty();
    return ok;
}

bool RegistryDocumentApplyService::saveOriginalBackup(const RegistryApplyPlan& plan,
    const QString& path, QString& error)
{
    error.clear();
    if (plan.viewBits != 0 && plan.viewBits != 32 && plan.viewBits != 64)
        return failure(error, QStringLiteral("Invalid registry original-backup view."));
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("KSword.RegistryChangeOriginals"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("viewBits"), plan.viewBits);
    QJsonArray segments;
    qint64 bytesTotal = 0;
    for (const auto& tree : plan.originalSubtrees) {
        if (tree.viewBits != plan.viewBits)
            return failure(error, QStringLiteral("Registry original-backup views do not match."));
        QByteArray bytes;
        if (!RegistryDocumentService::encodeBackup(tree, bytes, error))
            return false;
        bytesTotal += bytes.size();
        if (bytesTotal > kJournalLimit)
            return failure(error, QStringLiteral("Registry original backup exceeds the size budget."));
        segments.append(QJsonDocument::fromJson(bytes).object());
    }
    root.insert(QStringLiteral("segments"), segments);
    QJsonArray absent;
    for (const auto& key : plan.originallyAbsentKeys)
        absent.append(key);
    root.insert(QStringLiteral("originallyAbsentKeys"), absent);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > kJournalLimit)
        return failure(error, QStringLiteral("Registry original backup exceeds the size budget."));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return failure(error, QStringLiteral("Cannot save registry original backup: %1").arg(file.errorString()));
    return true;
}

bool RegistryDocumentApplyService::loadOriginalBackup(const QString& path,
    RegistryDocument& mergeDocument, QString& error)
{
    mergeDocument = {};
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return failure(error, QStringLiteral("Cannot open registry original backup: %1").arg(file.errorString()));
    if (file.size() < 0 || file.size() > kJournalLimit)
        return failure(error, QStringLiteral("Registry original backup exceeds the size budget."));
    const QByteArray bytes = file.read(kJournalLimit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > kJournalLimit || !file.atEnd())
        return failure(error, QStringLiteral("Cannot read the complete registry original backup."));
    const QJsonDocument json = QJsonDocument::fromJson(bytes);
    if (!json.isObject())
        return failure(error, QStringLiteral("Invalid registry original-backup JSON."));
    const auto root = json.object();
    const double view = root.value(QStringLiteral("viewBits")).toDouble(-1);
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("KSword.RegistryChangeOriginals")
        || root.value(QStringLiteral("version")).toDouble(-1) != 1
        || (view != 0 && view != 32 && view != 64)
        || !root.value(QStringLiteral("segments")).isArray()
        || !root.value(QStringLiteral("originallyAbsentKeys")).isArray())
        return failure(error, QStringLiteral("Unsupported registry original-backup format."));
    RegistryDocument loaded;
    loaded.viewBits = static_cast<int>(view);
    const auto segments = root.value(QStringLiteral("segments")).toArray();
    if (segments.size() > 100000)
        return failure(error, QStringLiteral("Registry original backup exceeds the segment count limit."));
    QStringList roots;
    qint64 dataBytes = 0;
    for (const auto& segment : segments) {
        if (!segment.isObject())
            return failure(error, QStringLiteral("Invalid registry original-backup segment."));
        RegistryDocument tree;
        if (!RegistryDocumentService::decodeBackup(QJsonDocument(segment.toObject()).toJson(QJsonDocument::Compact), tree, error))
            return false;
        if (tree.viewBits != loaded.viewBits)
            return failure(error, QStringLiteral("Registry original-backup views do not match."));
        QString normalizedRoot;
        if (!pathCanonical(tree.rootPath, normalizedRoot, error))
            return false;
        roots.append(normalizedRoot);
        for (auto& key : tree.keys) {
            dataBytes += key.securityDescriptor.size();
            // Restore is merge-by-default; OWNER/GROUP/DACL remains metadata.
            loaded.operationOrder.append({RegistryDocumentOperation::Kind::Key, loaded.keys.size()});
            loaded.keys.append(std::move(key));
        }
        for (auto& value : tree.values) {
            dataBytes += value.data.size();
            loaded.operationOrder.append({RegistryDocumentOperation::Kind::Value, loaded.values.size()});
            loaded.values.append(std::move(value));
        }
        if (dataBytes > kPlanDataLimit || loaded.keys.size() > 100000 || loaded.values.size() > 250000)
            return failure(error, QStringLiteral("Registry original restore exceeds the data or item budget."));
    }
    std::sort(roots.begin(), roots.end(), [](const QString& a, const QString& b) {
        return (a + QLatin1Char('\\')).compare(b + QLatin1Char('\\'), Qt::CaseInsensitive) < 0;
    });
    for (qsizetype i = 1; i < roots.size(); ++i) {
        if (inside(roots.at(i), roots.at(i - 1)))
            return failure(error, QStringLiteral("Registry original-backup segments overlap."));
    }
    const auto absent = root.value(QStringLiteral("originallyAbsentKeys")).toArray();
    if (absent.size() > 100000)
        return failure(error, QStringLiteral("Registry original backup exceeds the absent-key count limit."));
    for (const auto& entry : absent) {
        QString normalized;
        if (!entry.isString() || !pathCanonical(entry.toString(), normalized, error))
            return failure(error, QStringLiteral("Invalid absent-key metadata in registry original backup."));
    }
    if (loaded.keys.isEmpty())
        return failure(error, QStringLiteral("The original backup contains no existing data to merge-restore."));
    mergeDocument = std::move(loaded);
    return true;
}

#ifdef Q_OS_WIN
namespace
{
class ApplyKey final
{
public:
    ~ApplyKey() { close(); }
    void close() { if (handle) { RegCloseKey(handle); handle = nullptr; } }
    HKEY handle = nullptr;
};

bool apiError(QString& error, const QString& path, LSTATUS status)
{
    return failure(error, QStringLiteral("Registry operation failed at %1 (Win32 %2).").arg(path).arg(status));
}

class ApplyTransaction final
{
public:
    ~ApplyTransaction()
    {
        // Closing the last uncommitted KTM handle rolls the transaction back.
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (m_module) FreeLibrary(m_module);
    }
    bool begin(const QString& path, QString& error)
    {
        m_module = LoadLibraryExW(L"KtmW32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m_module) return apiError(error, path, GetLastError());
        using Create = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES, LPGUID, DWORD, DWORD, DWORD, DWORD, LPWSTR);
        Create create = nullptr;
        const FARPROC createAddress = GetProcAddress(m_module, "CreateTransaction");
        const FARPROC commitAddress = GetProcAddress(m_module, "CommitTransaction");
        static_assert(sizeof(create) == sizeof(createAddress));
        static_assert(sizeof(m_commit) == sizeof(commitAddress));
        std::memcpy(&create, &createAddress, sizeof(create));
        std::memcpy(&m_commit, &commitAddress, sizeof(m_commit));
        if (!create || !m_commit) return apiError(error, path, ERROR_PROC_NOT_FOUND);
        handle = create(nullptr, nullptr, 0, 0, 0, 30000, nullptr);
        return handle != INVALID_HANDLE_VALUE || apiError(error, path, GetLastError());
    }
    bool commit(const QString& path, QString& error)
    {
        return m_commit(handle) || apiError(error, path, GetLastError());
    }
    HANDLE handle = INVALID_HANDLE_VALUE;
private:
    HMODULE m_module = nullptr;
    BOOL (WINAPI* m_commit)(HANDLE) = nullptr;
};

HKEY rootHandle(const QString& root)
{
    if (root == QStringLiteral("HKEY_CLASSES_ROOT")) return HKEY_CLASSES_ROOT;
    if (root == QStringLiteral("HKEY_CURRENT_USER")) return HKEY_CURRENT_USER;
    if (root == QStringLiteral("HKEY_LOCAL_MACHINE")) return HKEY_LOCAL_MACHINE;
    if (root == QStringLiteral("HKEY_USERS")) return HKEY_USERS;
    if (root == QStringLiteral("HKEY_CURRENT_CONFIG")) return HKEY_CURRENT_CONFIG;
    return nullptr;
}

bool inspectLink(HKEY handle, const QString& path, QString& error)
{
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(handle, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr);
    if (status == ERROR_SUCCESS && type == REG_LINK)
        return failure(error, QStringLiteral("Registry operations refuse symbolic links: %1").arg(path));
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return apiError(error, path, status);
    return true;
}

bool keyIdentity(HKEY handle, const QString& path, QString& identity, QString& error)
{
    using QueryKey = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    QueryKey query = nullptr;
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryKey");
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    if (!query) return apiError(error, path, ERROR_PROC_NOT_FOUND);
    ULONG required = 0;
    // KeyNameInformation contains a byte count followed by the native UTF-16 name.
    query(handle, 3, nullptr, 0, &required);
    if (required < sizeof(ULONG) || required > 128 * 1024)
        return apiError(error, path, ERROR_INVALID_DATA);
    QByteArray buffer(static_cast<qsizetype>(required), Qt::Uninitialized);
    ULONG returned = required;
    if (query(handle, 3, buffer.data(), required, &returned) < 0)
        return apiError(error, path, ERROR_INVALID_HANDLE);
    ULONG bytes = 0;
    std::memcpy(&bytes, buffer.constData(), sizeof(bytes));
    if (returned < sizeof(bytes) || returned > required || !bytes || bytes > returned - sizeof(bytes) || bytes % 2)
        return apiError(error, path, ERROR_INVALID_DATA);
    identity = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(buffer.constData() + sizeof(bytes)), bytes / 2);
    return true;
}

class Win32ApplyBackend final : public RegistryApplyBackend
{
public:
    explicit Win32ApplyBackend(int viewBits, const std::atomic_bool* canceled = nullptr) : m_bits(viewBits),
        m_view(viewBits == 32 ? KEY_WOW64_32KEY : viewBits == 64 ? KEY_WOW64_64KEY : 0), m_canceled(canceled) {}

    bool keyExists(const QString& path, bool& exists, QString& error) override
    {
        ApplyKey key;
        LSTATUS status = ERROR_SUCCESS;
        if (!open(path, KEY_QUERY_VALUE, key, status, error)) {
            if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND || status == ERROR_KEY_DELETED) {
                exists = false;
                error.clear();
                return true;
            }
            return false;
        }
        exists = true;
        return true;
    }

    bool readValue(const QString& path, const QString& name, RegistryApplyValueState& value, QString& error) override
    {
        value = {};
        ApplyKey key;
        LSTATUS status = ERROR_SUCCESS;
        if (!open(path, KEY_QUERY_VALUE, key, status, error))
            return false;
        return readValueHandle(key.handle, path, name, value, error);
    }

    bool readValueHandle(HKEY handle, const QString& path, const QString& name,
        RegistryApplyValueState& value, QString& error)
    {
        value = {};
        DWORD type = 0, size = 0;
        LSTATUS status = RegQueryValueExW(handle, reinterpret_cast<LPCWSTR>(name.utf16()), nullptr, &type, nullptr, &size);
        if (status == ERROR_FILE_NOT_FOUND)
            return true;
        if (status != ERROR_SUCCESS)
            return apiError(error, path, status);
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (size > 16 * 1024 * 1024)
                return failure(error, QStringLiteral("Registry value exceeds the apply read budget."));
            QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
            DWORD actual = size;
            status = RegQueryValueExW(handle, reinterpret_cast<LPCWSTR>(name.utf16()), nullptr,
                &type, reinterpret_cast<BYTE*>(data.data()), &actual);
            if (status == ERROR_MORE_DATA) { size = actual; continue; }
            if (status == ERROR_FILE_NOT_FOUND)
                return true;
            if (status != ERROR_SUCCESS)
                return apiError(error, path, status);
            if (actual > static_cast<DWORD>(data.size()))
                return failure(error, QStringLiteral("Registry apply read returned an invalid data length."));
            data.resize(actual);
            value = {true, type, std::move(data)};
            return true;
        }
        return failure(error, QStringLiteral("Registry value kept growing during apply verification."));
    }

    bool captureTree(const QString& path, RegistryDocument& tree, QString& error) override
    {
        return captureTreeBounded(path, kPlanDataLimit, tree, error);
    }

    bool captureTreeBounded(const QString& path, qint64 maximumDataBytes,
        RegistryDocument& tree, QString& error) override
    {
        ApplyKey key;
        LSTATUS status = ERROR_SUCCESS;
        if (!open(path, KEY_QUERY_VALUE, key, status, error))
            return false;
        return RegistryDocumentService::captureWin32(path, m_bits, tree, error, maximumDataBytes);
    }

    bool createKey(const QString& path, QString& error) override
    {
        const QString parent = parentPath(path);
        if (parent.isEmpty())
            return failure(error, QStringLiteral("A predefined registry root cannot be created."));
        ApplyKey parentKey, created;
        LSTATUS status = ERROR_SUCCESS;
        if (!open(parent, KEY_CREATE_SUB_KEY | KEY_QUERY_VALUE, parentKey, status, error))
            return false;
        const QString name = path.mid(parent.size() + 1);
        DWORD disposition = 0;
        if (canceled(error))
            return false;
        status = RegCreateKeyExW(parentKey.handle, reinterpret_cast<LPCWSTR>(name.utf16()), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | m_view, nullptr, &created.handle, &disposition);
        if (status != ERROR_SUCCESS)
            return apiError(error, path, status);
        if (disposition != REG_CREATED_NEW_KEY)
            return failure(error, QStringLiteral("Registry key appeared after its creation preview."));
        return true;
    }

    bool setValue(const QString& path, const QString& name, quint32 type, const QByteArray& data, QString& error) override
    {
        ApplyKey key;
        LSTATUS status = ERROR_SUCCESS;
        if (!open(path, KEY_QUERY_VALUE | KEY_SET_VALUE, key, status, error))
            return false;
        if (canceled(error))
            return false;
        status = RegSetValueExW(key.handle, reinterpret_cast<LPCWSTR>(name.utf16()), 0, type,
            reinterpret_cast<const BYTE*>(data.constData()), static_cast<DWORD>(data.size()));
        return status == ERROR_SUCCESS || apiError(error, path, status);
    }

    bool deleteValue(const QString& path, const QString& name, QString& error) override
    {
        ApplyKey key;
        LSTATUS status = ERROR_SUCCESS;
        if (!open(path, KEY_QUERY_VALUE | KEY_SET_VALUE, key, status, error))
            return false;
        if (canceled(error))
            return false;
        status = RegDeleteValueW(key.handle, reinterpret_cast<LPCWSTR>(name.utf16()));
        // Missing after the immediately preceding comparison is a conflict.
        return status == ERROR_SUCCESS || apiError(error, path, status);
    }

    bool deleteTree(const QString& path, const RegistryDocument& expectedTree, QString& error) override
    {
        if (parentPath(path).isEmpty() || expectedTree.rootPath.compare(path, Qt::CaseInsensitive) != 0
            || expectedTree.keys.isEmpty() || hasLink(expectedTree))
            return failure(error, QStringLiteral("Deleting a predefined registry root is forbidden."));
        QStringList paths;
        for (const auto& key : expectedTree.keys) {
            if (!inside(key.path, path))
                return failure(error, QStringLiteral("Registry deletion plan contains a key outside its authorized subtree."));
            paths.append(key.path);
        }
        std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
        QHash<QString, QVector<RegistryDocumentValue>> values;
        QHash<QString, QByteArray> security;
        for (const auto& key : expectedTree.keys) security.insert(folded(key.path), key.securityDescriptor);
        for (const auto& value : expectedTree.values)
            values[folded(value.keyPath)].append(value);
        ApplyTransaction transaction;
        if (!transaction.begin(path, error)) return false;
        // This list is fixed before the first write. Newly discovered children
        // never extend it. All checks and deletions share one transaction; an
        // outside writer either conflicts or aborts our commit, retaining its data.
        for (const auto& target : paths) {
            if (canceled(error))
                return false;
            ApplyKey key, parent;
            LSTATUS status = ERROR_SUCCESS;
            const auto expectedSecurity = security.value(folded(target));
            if (!openTransacted(target, KEY_QUERY_VALUE | DELETE | (expectedSecurity.isEmpty() ? 0 : READ_CONTROL),
                transaction.handle, key, status, error))
                return false;
            if (!expectedSecurity.isEmpty()) {
                constexpr SECURITY_INFORMATION parts = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
                DWORD bytes = 0;
                status = RegGetKeySecurity(key.handle, parts, nullptr, &bytes);
                if (status != ERROR_INSUFFICIENT_BUFFER || bytes > 1024 * 1024 || bytes == 0)
                    return apiError(error, target, status == ERROR_INSUFFICIENT_BUFFER || status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status);
                QByteArray currentSecurity(static_cast<qsizetype>(bytes), Qt::Uninitialized);
                status = RegGetKeySecurity(key.handle, parts,
                    reinterpret_cast<PSECURITY_DESCRIPTOR>(currentSecurity.data()), &bytes);
                if (status != ERROR_SUCCESS) return apiError(error, target, status);
                if (bytes > static_cast<DWORD>(currentSecurity.size())) return apiError(error, target, ERROR_INVALID_DATA);
                currentSecurity.resize(bytes);
                if (currentSecurity != expectedSecurity) return apiError(error, target, ERROR_TRANSACTIONAL_CONFLICT);
            }
            DWORD childCount = 0, valueCount = 0;
            status = RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &childCount, nullptr,
                nullptr, &valueCount, nullptr, nullptr, nullptr, nullptr);
            if (status != ERROR_SUCCESS)
                return apiError(error, target, status);
            const auto expectedValues = values.value(folded(target));
            if (childCount != 0 || valueCount != static_cast<DWORD>(expectedValues.size()))
                return failure(error, QStringLiteral("Registry deletion stopped because the key acquired a child or changed its values."));
            for (const auto& value : expectedValues) {
                RegistryApplyValueState actual;
                if (!readValueHandle(key.handle, target, value.name, actual, error))
                    return false;
                if (!sameValue({true, value.type, value.data}, actual))
                    return failure(error, QStringLiteral("Registry value changed immediately before tree deletion."));
            }
            const QString parentName = parentPath(target);
            if (!openTransacted(parentName, KEY_QUERY_VALUE, transaction.handle, parent, status, error))
                return false;
            const QString leaf = target.mid(parentName.size() + 1);
            ApplyKey candidate;
            status = RegOpenKeyTransactedW(parent.handle, reinterpret_cast<LPCWSTR>(leaf.utf16()), 0,
                KEY_QUERY_VALUE | DELETE | m_view, &candidate.handle, transaction.handle, nullptr);
            if (status != ERROR_SUCCESS) return apiError(error, target, status);
            // Bind the name used by deletion as well as the no-follow object that
            // was checked. A rename/replacement/link race must not redirect the
            // parent-relative delete to a different key with unbacked values.
            QString expectedIdentity, candidateIdentity;
            if (!keyIdentity(key.handle, target, expectedIdentity, error)
                || !keyIdentity(candidate.handle, target, candidateIdentity, error)) return false;
            if (expectedIdentity.compare(candidateIdentity, Qt::CaseInsensitive) != 0)
                return apiError(error, target, ERROR_TRANSACTIONAL_CONFLICT);
            key.close();
            if (canceled(error))
                return false;
            status = RegDeleteKeyTransactedW(parent.handle, reinterpret_cast<LPCWSTR>(leaf.utf16()),
                m_view, 0, transaction.handle, nullptr);
            if (status != ERROR_SUCCESS)
                return apiError(error, target, status);
        }
        if (canceled(error)) return false;
        return transaction.commit(path, error);
    }

private:
    bool openTransacted(const QString& path, REGSAM access, HANDLE transaction,
        ApplyKey& key, LSTATUS& status, QString& error)
    {
        // First obtain a no-follow handle and inspect every path component. The
        // transacted API reserves its options parameter, so bind the transaction
        // to this exact handle through an empty subkey instead of traversing again.
        ApplyKey exact;
        if (!open(path, access, exact, status, error)) return false;
        status = RegOpenKeyTransactedW(exact.handle, L"", 0,
            access | KEY_QUERY_VALUE | m_view, &key.handle, transaction, nullptr);
        return status == ERROR_SUCCESS || apiError(error, path, status);
    }

    bool canceled(QString& error) const
    {
        if (!m_canceled || !m_canceled->load(std::memory_order_relaxed))
            return false;
        error = QStringLiteral("Registry operation canceled before writing.");
        return true;
    }
    bool open(const QString& input, REGSAM access, ApplyKey& key, LSTATUS& status, QString& error)
    {
        QString path;
        if (!pathCanonical(input, path, error)) { status = ERROR_INVALID_PARAMETER; return false; }
        const auto parts = path.split(QLatin1Char('\\'));
        HKEY current = rootHandle(parts.first());
        ApplyKey owned;
        QString currentPath = parts.first();
        if (parts.size() == 1) {
            status = RegOpenKeyExW(current, L"", REG_OPTION_OPEN_LINK, access | KEY_QUERY_VALUE | m_view, &key.handle);
            return status == ERROR_SUCCESS || apiError(error, path, status);
        }
        for (qsizetype i = 1; i < parts.size(); ++i) {
            HKEY child = nullptr;
            status = RegOpenKeyExW(current, reinterpret_cast<LPCWSTR>(parts.at(i).utf16()), REG_OPTION_OPEN_LINK,
                (i + 1 == parts.size() ? access : KEY_QUERY_VALUE) | KEY_QUERY_VALUE | m_view, &child);
            if (status != ERROR_SUCCESS)
                return apiError(error, path, status);
            owned.close();
            owned.handle = child;
            current = child;
            currentPath += QLatin1Char('\\') + parts.at(i);
            if (!inspectLink(current, currentPath, error)) { status = ERROR_INVALID_PARAMETER; return false; }
        }
        key.handle = owned.handle;
        owned.handle = nullptr;
        return true;
    }

    int m_bits = 0;
    REGSAM m_view = 0;
    const std::atomic_bool* m_canceled = nullptr;
};
} // namespace
#endif

bool RegistryDocumentApplyService::prepareWin32(const RegistryDocument& document, RegistryApplyPlan& plan, QString& error)
{
#ifdef Q_OS_WIN
    Win32ApplyBackend backend(document.viewBits);
    return prepareWithBackend(document, backend, plan, error);
#else
    Q_UNUSED(document);
    plan = {};
    return failure(error, QStringLiteral("Registry apply preparation requires Windows."));
#endif
}

bool RegistryDocumentApplyService::applyWin32(const RegistryApplyPlan& plan, RegistryApplyResult& result,
    const std::atomic_bool* canceledToken)
{
#ifdef Q_OS_WIN
    Win32ApplyBackend backend(plan.viewBits, canceledToken);
    return applyWithBackend(plan, backend, result, canceledToken);
#else
    Q_UNUSED(plan); Q_UNUSED(canceledToken);
    result = {};
    return failure(result.error, QStringLiteral("Registry application requires Windows."));
#endif
}

bool RegistryDocumentApplyService::undoWin32(const RegistryApplyResult& previous, RegistryApplyResult& result,
    const std::atomic_bool* canceledToken)
{
#ifdef Q_OS_WIN
    Win32ApplyBackend backend(previous.viewBits, canceledToken);
    return undoWithBackend(previous, backend, result, canceledToken);
#else
    Q_UNUSED(previous); Q_UNUSED(canceledToken);
    result = {};
    return failure(result.error, QStringLiteral("Registry committed undo requires Windows."));
#endif
}
