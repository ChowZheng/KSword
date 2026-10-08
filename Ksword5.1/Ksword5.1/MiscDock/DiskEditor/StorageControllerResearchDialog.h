#pragma once

#include "../../ArkDriverClient/ArkStorageControllerClient.h"

#include <QByteArray>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QShowEvent;

namespace ks::misc
{
    // Retain the historical source/class name; this is now an embedded File tab page.
    class StorageControllerResearchDialog final : public QWidget
    {
    public:
        explicit StorageControllerResearchDialog(QWidget* parent = nullptr);
        ~StorageControllerResearchDialog() override = default;

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        template<typename Work, typename Apply>
        void runOperation(Work work, Apply apply);
        void initializeUi();
        void refreshState();
        void acquireSession();
        void releaseSession();
        void resetController();
        void readRange();
        void writeRange();
        void rollbackRange();
        void refreshAudit();
        void applyQuery(const ksword::ark::StorageControllerQueryResult& result);
        void invalidateQueryState();
        void clearSnapshot();
        bool parseRange(std::uint64_t& offset, std::uint32_t& length) const;
        bool confirmDangerousOperation(const QString& title, const QString& detail);
        void appendLog(const QString& message);
        void updateActionState();
        static QString ownershipText(unsigned long ownership);
        static QString coherencyText(unsigned long coherency);
        static QString riskText(unsigned long flags);
        static QString controllerTypeText(unsigned long type);
        static QString hashText(const unsigned char* hash);

        // A worker lease keeps the handle alive until its IOCTL completes. Widget
        // destruction disconnects the watcher and never closes an active handle.
        std::shared_ptr<ksword::ark::ArkStorageControllerClient> m_client;
        std::uint32_t m_capabilities = 0U;
        std::uint32_t m_generation = 0U;
        std::uint32_t m_sectorSize = 0U;
        std::uint32_t m_maximumTransfer = 0U;
        std::uint64_t m_capacity = 0ULL;
        std::uint64_t m_sessionId = 0ULL;
        std::uint64_t m_snapshotOffset = 0ULL;
        std::uint32_t m_snapshotLength = 0U;
        std::uint64_t m_rollbackOffset = 0ULL;
        std::uint32_t m_rollbackLength = 0U;
        bool m_rollbackValid = false;
        bool m_resetRequired = false;
        bool m_resetSupported = false;
        bool m_connected = false;
        bool m_queryValid = false;
        bool m_ready = false;
        bool m_busy = false;
        bool m_confirming = false;
        bool m_initialized = false;
        std::vector<std::uint8_t> m_snapshotHash;
        QLabel* m_riskLabel = nullptr;
        QLabel* m_identityLabel = nullptr;
        QLabel* m_stateLabel = nullptr;
        QLineEdit* m_offsetEdit = nullptr;
        QLineEdit* m_lengthEdit = nullptr;
        QPlainTextEdit* m_hexEdit = nullptr;
        QPlainTextEdit* m_logEdit = nullptr;
        QTableWidget* m_auditTable = nullptr;
        QPushButton* m_refreshButton = nullptr;
        QPushButton* m_auditButton = nullptr;
        QPushButton* m_acquireButton = nullptr;
        QPushButton* m_releaseButton = nullptr;
        QPushButton* m_resetButton = nullptr;
        QPushButton* m_readButton = nullptr;
        QPushButton* m_writeButton = nullptr;
        QPushButton* m_rollbackButton = nullptr;
    };
}
