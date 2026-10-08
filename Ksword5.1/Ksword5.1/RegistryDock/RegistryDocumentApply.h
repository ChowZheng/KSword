#pragma once

#include "RegistryDocument.h"
#include <QStringList>
#include <atomic>

struct RegistryApplyValueState
{
    bool exists = false;
    quint32 type = 0;
    QByteArray data;
};

struct RegistryApplyOperation
{
    enum class Kind { CreateKey, DeleteTree, SetValue, DeleteValue };
    Kind kind = Kind::CreateKey;
    QString keyPath;
    QString valueName;
    bool keyExistedBefore = false;
    RegistryApplyValueState beforeValue;
    RegistryApplyValueState afterValue;
    RegistryDocument beforeTree;
};

struct RegistryApplyPlan
{
    int viewBits = 0;
    QVector<RegistryApplyOperation> operations;
    QVector<RegistryDocument> originalSubtrees;
    QStringList originallyAbsentKeys;
};

struct RegistryApplyReceipt
{
    enum class State { NotRun, Success, Failed, Canceled };
    RegistryApplyOperation operation;
    State state = State::NotRun;
    bool mutated = false;
    RegistryApplyValueState actualAfterValue;
    RegistryDocument actualAfterTree;
    QString error;
};

struct RegistryApplyResult
{
    int viewBits = 0;
    bool completed = false;
    bool canceled = false;
    // A deletion of an existing tree requires backup restoration instead of an
    // automatic undo that might silently discard its original ACL inheritance.
    bool canUndo = false;
    QString error;
    QVector<RegistryApplyReceipt> receipts;
    // An undo retry must continue the original commit, never invert the receipts
    // of a partially successful undo (which would redo already restored values).
    bool undoAttempt = false;
    QVector<RegistryApplyReceipt> pendingUndoReceipts;
};

// Small injectable boundary for file/codec and state-machine tests. Implementations
// must preserve raw type/data and must never follow registry symbolic links.
class RegistryApplyBackend
{
public:
    virtual ~RegistryApplyBackend() = default;
    virtual bool keyExists(const QString& path, bool& exists, QString& error) = 0;
    virtual bool readValue(const QString& path, const QString& name, RegistryApplyValueState& value, QString& error) = 0;
    virtual bool captureTree(const QString& path, RegistryDocument& tree, QString& error) = 0;
    virtual bool captureTreeBounded(const QString& path, qint64 maximumDataBytes,
        RegistryDocument& tree, QString& error);
    virtual bool createKey(const QString& path, QString& error) = 0;
    virtual bool setValue(const QString& path, const QString& name, quint32 type, const QByteArray& data, QString& error) = 0;
    virtual bool deleteValue(const QString& path, const QString& name, QString& error) = 0;
    virtual bool deleteTree(const QString& path, const RegistryDocument& expectedTree, QString& error) = 0;
};

class RegistryDocumentApplyService final
{
public:
    // Preparation is read-only. A host must display the plan before applying it.
    // Restore documents are merged; data outside their listed operations is retained.
    static bool prepareWin32(const RegistryDocument& document, RegistryApplyPlan& plan, QString& error);
    static bool applyWin32(const RegistryApplyPlan& plan, RegistryApplyResult& result,
        const std::atomic_bool* canceledToken = nullptr);
    static bool undoWin32(const RegistryApplyResult& previous, RegistryApplyResult& result,
        const std::atomic_bool* canceledToken = nullptr);
    // Journals store every original complete subtree plus absent-key metadata.
    // Loading yields a MERGE restore document; it never deletes newly added data.
    static bool saveOriginalBackup(const RegistryApplyPlan& plan, const QString& path, QString& error);
    static bool loadOriginalBackup(const QString& path, RegistryDocument& mergeDocument, QString& error);

    static bool prepareWithBackend(const RegistryDocument& document, RegistryApplyBackend& backend,
        RegistryApplyPlan& plan, QString& error);
    static bool applyWithBackend(const RegistryApplyPlan& plan, RegistryApplyBackend& backend,
        RegistryApplyResult& result, const std::atomic_bool* canceledToken = nullptr);
    static bool undoWithBackend(const RegistryApplyResult& previous, RegistryApplyBackend& backend,
        RegistryApplyResult& result, const std::atomic_bool* canceledToken = nullptr);
};
