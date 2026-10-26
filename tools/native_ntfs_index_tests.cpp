// Native Windows/NTFS contract test. The runner requires a dedicated test volume.
#include "NtfsFastIndex.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using ks::files::NtfsFastIndex;
using ks::files::NtfsIndexEntry;
using ks::files::NtfsIndexSnapshot;
int checks = 0;
void require(bool condition, const QString& message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message.toStdString());
}
QString native(QString path) { return QDir::toNativeSeparators(QDir::cleanPath(path)); }
bool samePath(const QString& a, const QString& b)
{ return native(a).compare(native(b), Qt::CaseInsensitive) == 0; }
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
};
void writeFile(const QString& path, const QByteArray& bytes)
{
    const std::wstring name = path.toStdWString();
    Handle file{::CreateFileW(name.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(file.value != INVALID_HANDLE_VALUE, QStringLiteral("CreateFile failed: ") + path);
    DWORD written = 0;
    require(::WriteFile(file.value, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == static_cast<DWORD>(bytes.size()), QStringLiteral("WriteFile failed: ") + path);
    require(::FlushFileBuffers(file.value) != FALSE, QStringLiteral("FlushFileBuffers failed: ") + path);
}
quint64 fileReference(const QString& path)
{
    const std::wstring name = path.toStdWString();
    Handle file{::CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    require(file.value != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(file.value, &info),
        QStringLiteral("Cannot query native file identity: ") + path);
    return (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}
quint64 allocatedBytes(const QString& path)
{
    const std::wstring name = path.toStdWString();
    Handle file{::CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    FILE_STANDARD_INFO info{};
    require(file.value != INVALID_HANDLE_VALUE
        && ::GetFileInformationByHandleEx(file.value, FileStandardInfo, &info, sizeof(info))
        && info.AllocationSize.QuadPart >= 0,
        QStringLiteral("Cannot query native allocated size: ") + path);
    return static_cast<quint64>(info.AllocationSize.QuadPart);
}
QStringList aliases(const NtfsIndexSnapshot& snapshot, const NtfsIndexEntry& entry)
{
    QStringList paths;
    for (const auto& name : entry.names) {
        const QString parent = NtfsFastIndex::fullPath(snapshot, name.parentReference);
        if (!parent.isEmpty()) paths.push_back(native(QDir(parent).filePath(name.name)));
    }
    return paths;
}
bool containsPath(const QStringList& paths, const QString& path)
{
    return std::any_of(paths.cbegin(), paths.cend(), [&](const QString& item) { return samePath(item, path); });
}
QByteArray fingerprint(const NtfsIndexSnapshot& snapshot)
{
    QByteArray bytes;
    QDataStream data(&bytes, QIODevice::WriteOnly);
    data << snapshot.volumeRoot << snapshot.volumeSerial << snapshot.rootReference
         << snapshot.journalId << snapshot.nextUsn << snapshot.unknownSizeCount << snapshot.namesIncompleteCount;
    const auto names = [&](const auto& values) {
        data << static_cast<quint64>(values.size());
        for (const auto& value : values) data << value.parentReference << value.name;
    };
    auto keys = snapshot.entries.keys();
    std::sort(keys.begin(), keys.end());
    data << static_cast<quint64>(keys.size());
    for (const quint64 key : keys) {
        const auto& entry = snapshot.entries[key];
        data << key << entry.fileReference << entry.parentReference << entry.name
             << entry.attributes << entry.logicalBytes << entry.allocatedBytes << entry.sizeKnown
             << entry.hasAttributeList << entry.attributeListResident << entry.attributeReferences;
        names(entry.names); names(entry.baseNames);
    }
    auto extensions = snapshot.extensions.keys();
    std::sort(extensions.begin(), extensions.end());
    data << static_cast<quint64>(extensions.size());
    for (const quint64 key : extensions) {
        const auto& extension = snapshot.extensions[key];
        data << key << extension.baseReference;
        names(extension.names);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
void ready(const NtfsFastIndex::Snapshot& snapshot, const QString& operation)
{
    require(snapshot && snapshot->complete && !snapshot->cancelled && snapshot->error.isEmpty(),
        operation + QStringLiteral(": ") + (snapshot ? snapshot->error : QStringLiteral("null snapshot")));
    require(snapshot->journalAvailable, QStringLiteral("The dedicated NTFS test volume must have an active USN Journal"));
}
// Failure-only metadata probes. Known fixture identities avoid an unbounded
// Journal walk and never print names, paths, attribute payloads or file data.
void diagnoseRefresh(const QString& root, qint64 initialCursor,
    const std::array<quint64, 3>& knownReferences) noexcept
{
    try {
        std::fprintf(stderr, "NATIVE_NTFS_REFRESH_DIAGNOSTIC initial_cursor=%lld known_records=%llu\n",
            static_cast<long long>(initialCursor), static_cast<unsigned long long>(knownReferences.size()));
        const std::wstring device = (QStringLiteral("\\\\.\\") + root.left(2)).toStdWString();
        Handle volume{::CreateFileW(device.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
        if (volume.value == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_OPEN_ERROR=%lu\n", ::GetLastError()); return;
        }
        NTFS_VOLUME_DATA_BUFFER geometry{};
        DWORD returned = 0;
        if (!::DeviceIoControl(volume.value, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                &geometry, sizeof(geometry), &returned, nullptr)) {
            std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_GEOMETRY_ERROR=%lu\n", ::GetLastError()); return;
        }
        if (returned < sizeof(geometry) || returned > sizeof(geometry)
            || geometry.BytesPerFileRecordSegment < 48 || geometry.BytesPerFileRecordSegment > 65536
            || geometry.BytesPerSector < 512 || geometry.BytesPerSector > 65536
            || (geometry.BytesPerSector & (geometry.BytesPerSector - 1)) != 0) {
            std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_GEOMETRY_BOUNDS=INVALID\n"); return;
        }
        constexpr quint64 indexMask = 0x0000ffffffffffffULL;
        const std::size_t header = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer);
        const std::size_t maximumRecords = std::min<std::size_t>(8, knownReferences.size());
        for (std::size_t index = 0; index < maximumRecords; ++index) {
            const quint64 requested = knownReferences[index];
            NTFS_FILE_RECORD_INPUT_BUFFER input{};
            input.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(requested & indexMask);
            std::vector<std::byte> buffer(header + geometry.BytesPerFileRecordSegment);
            returned = 0;
            if (!::DeviceIoControl(volume.value, FSCTL_GET_NTFS_FILE_RECORD, &input, sizeof(input),
                    buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
                std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_RECORD requested=%016llx error=%lu\n",
                    static_cast<unsigned long long>(requested), ::GetLastError());
                continue;
            }
            if (returned < header || returned > buffer.size()) {
                std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_RECORD requested=%016llx returned_bounds=INVALID\n",
                    static_cast<unsigned long long>(requested)); continue;
            }
            NTFS_FILE_RECORD_OUTPUT_BUFFER output{};
            std::memcpy(&output, buffer.data(), header);
            if (output.FileRecordLength < 48 || output.FileRecordLength > geometry.BytesPerFileRecordSegment
                || output.FileRecordLength > returned - header) {
                std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_RECORD requested=%016llx returned=%016llx length_bounds=INVALID\n",
                    static_cast<unsigned long long>(requested),
                    static_cast<unsigned long long>(output.FileReferenceNumber.QuadPart)); continue;
            }
            const std::byte* record = buffer.data() + header;
            const std::size_t length = output.FileRecordLength;
            const auto load = [record, length](std::size_t offset, std::size_t width, quint64& value) {
                if (offset > length || width > length - offset) return false;
                value = 0;
                for (std::size_t byte = 0; byte < width; ++byte)
                    value |= static_cast<quint64>(record[offset + byte]) << (byte * 8);
                return true;
            };
            quint64 flags = 0, used = 0, allocated = 0, firstAttribute = 0, usaOffset = 0, usaCount = 0, sequence = 0;
            if (!load(22, 2, flags) || !load(24, 4, used) || !load(28, 4, allocated)
                || !load(20, 2, firstAttribute) || !load(4, 2, usaOffset) || !load(6, 2, usaCount)
                || !load(16, 2, sequence)) continue;
            std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_RECORD requested=%016llx returned=%016llx sequence=%04llx flags=%04llx used=%llu allocated=%llu firstattr=%llu usa_offset=%llu usa_count=%llu record_bytes=%llu sector_bytes=%lu\n",
                static_cast<unsigned long long>(requested), static_cast<unsigned long long>(output.FileReferenceNumber.QuadPart),
                static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(flags),
                static_cast<unsigned long long>(used), static_cast<unsigned long long>(allocated),
                static_cast<unsigned long long>(firstAttribute), static_cast<unsigned long long>(usaOffset),
                static_cast<unsigned long long>(usaCount), static_cast<unsigned long long>(length), geometry.BytesPerSector);
            quint64 updateSequence = 0;
            if (usaCount < 1 || usaOffset > length || usaCount > (length - usaOffset) / 2
                || !load(static_cast<std::size_t>(usaOffset), 2, updateSequence)) {
                std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_USA_BOUNDS=INVALID\n"); continue;
            }
            const quint64 sectors = std::min<quint64>(8, std::min<quint64>(usaCount - 1, length / geometry.BytesPerSector));
            for (quint64 sector = 0; sector < sectors; ++sector) {
                quint64 tail = 0, replacement = 0;
                const auto tailOffset = static_cast<std::size_t>((sector + 1) * geometry.BytesPerSector - 2);
                const auto replacementOffset = static_cast<std::size_t>(usaOffset + (sector + 1) * 2);
                if (!load(tailOffset, 2, tail) || !load(replacementOffset, 2, replacement)) break;
                std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_SECTOR index=%llu tail=%04llx update_sequence=%04llx replacement=%04llx\n",
                    static_cast<unsigned long long>(sector), static_cast<unsigned long long>(tail),
                    static_cast<unsigned long long>(updateSequence), static_cast<unsigned long long>(replacement));
            }
        }
    } catch (...) {
        std::fprintf(stderr, "NATIVE_NTFS_DIAGNOSTIC_ABORTED=1\n");
    }
}
void readyRefresh(const NtfsFastIndex::Snapshot& snapshot, const QString& operation,
    const QString& root, qint64 initialCursor, const std::array<quint64, 3>& knownReferences)
{
    if (!snapshot || !snapshot->complete || snapshot->cancelled || !snapshot->error.isEmpty() || !snapshot->journalAvailable)
        diagnoseRefresh(root, initialCursor, knownReferences);
    ready(snapshot, operation);
}
void validateVolume(const QString& root, const QString& work)
{
    require(root.size() == 3 && root.at(0).isLetter() && root.mid(1) == QStringLiteral(":\\"),
        QStringLiteral("Pass a dedicated drive root, such as X:\\"));
    const std::wstring rootName = root.toStdWString();
    WCHAR filesystem[MAX_PATH]{};
    require(::GetVolumeInformationW(rootName.c_str(), nullptr, 0, nullptr, nullptr, nullptr, filesystem, MAX_PATH)
        && QString::fromWCharArray(filesystem).compare(QStringLiteral("NTFS"), Qt::CaseInsensitive) == 0,
        QStringLiteral("The test volume must use NTFS"));
    WCHAR windows[MAX_PATH]{}, systemRoot[MAX_PATH]{}, systemVolume[MAX_PATH]{}, testVolume[MAX_PATH]{};
    require(::GetWindowsDirectoryW(windows, MAX_PATH) != 0
        && ::GetVolumePathNameW(windows, systemRoot, MAX_PATH)
        && ::GetVolumeNameForVolumeMountPointW(systemRoot, systemVolume, MAX_PATH)
        && ::GetVolumeNameForVolumeMountPointW(rootName.c_str(), testVolume, MAX_PATH),
        QStringLiteral("Cannot validate the dedicated volume identity"));
    require(QString::fromWCharArray(systemVolume).compare(QString::fromWCharArray(testVolume), Qt::CaseInsensitive) != 0,
        QStringLiteral("The Windows system volume is never a test target"));
    require(work.startsWith(root, Qt::CaseInsensitive) && !samePath(root, work),
        QStringLiteral("The work directory must be below the dedicated volume root"));
    for (QString path = work; !samePath(path, root); path = native(QDir(path).absoluteFilePath(QStringLiteral("..")))) {
        const std::wstring name = path.toStdWString();
        const DWORD attributes = ::GetFileAttributesW(name.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
            && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            QStringLiteral("The work directory and its parents must be ordinary existing directories"));
    }
}
void exercise(const QString& root, const QString& work)
{
    validateVolume(root, work);
    QTemporaryDir temporary(QDir(work).filePath(QStringLiteral("native-ntfs-XXXXXX")));
    require(temporary.isValid(), QStringLiteral("Cannot create the unique native test directory"));
    const QDir directory(temporary.path());
    require(directory.mkdir(QStringLiteral("a")) && directory.mkdir(QStringLiteral("b")), QStringLiteral("Cannot create fixture folders"));
    const QString original = native(directory.filePath(QStringLiteral("a/payload.bin")));
    const QString renamed = native(directory.filePath(QStringLiteral("a/renamed.bin")));
    const QString link = native(directory.filePath(QStringLiteral("b/payload-link.bin")));
    const QString secondLink = native(directory.filePath(QStringLiteral("a/payload-second.bin")));
    const QString removed = native(directory.filePath(QStringLiteral("delete-me.bin")));
    const QString smallFilePath = native(directory.filePath(QStringLiteral("small.txt")));
    const QString duringScan = native(directory.filePath(QStringLiteral("during-scan.bin")));
    const QByteArray payload(128 * 1024 + 73, 'Q');
    writeFile(original, payload); writeFile(removed, QByteArray(8192, 'D')); writeFile(smallFilePath, QByteArray(17, 's'));
    require(::CreateHardLinkW(link.toStdWString().c_str(), original.toStdWString().c_str(), nullptr)
        && ::CreateHardLinkW(secondLink.toStdWString().c_str(), original.toStdWString().c_str(), nullptr),
        QStringLiteral("Cannot create real NTFS hard links"));
    const quint64 reference = fileReference(original), removedReference = fileReference(removed);
    // Reserve live MFT slots, then free earlier slots. A file created during the
    // scan changes MFT metadata without requiring MFT growth on the test volume.
    for (int i = 0; i < 256; ++i) writeFile(directory.filePath(QStringLiteral("reserve-%1.bin").arg(i)), {});
    for (int i = 0; i < 128; ++i)
        require(QFile::remove(directory.filePath(QStringLiteral("reserve-%1.bin").arg(i))), QStringLiteral("Cannot release reserved MFT slots"));
    bool createdDuringScan = false;
    const auto initial = NtfsFastIndex::build(root, {}, [&](quint64, quint64) {
        if (!createdDuringScan) { createdDuringScan = true; writeFile(duringScan, QByteArray(4096, 'J')); }
    });
    ready(initial, QStringLiteral("Native MFT build failed"));
    require(createdDuringScan, QStringLiteral("Build progress was not delivered"));
    const quint64 duringScanReference = fileReference(duringScan);
    require(initial->entries.contains(duringScanReference), QStringLiteral("USN replay missed the file created during the raw scan"));
    require(initial->entries.contains(reference), QStringLiteral("MFT search missed the native payload identity"));
    const auto& entry = initial->entries[reference];
    const auto paths = aliases(*initial, entry);
    require(paths.size() == 3 && containsPath(paths, original) && containsPath(paths, link) && containsPath(paths, secondLink),
        QStringLiteral("MFT search did not retain all three hard-link names"));
    require(containsPath(paths, NtfsFastIndex::fullPath(*initial, reference)), QStringLiteral("Canonical fullPath is not one of the real hard links"));
    require(entry.sizeKnown && entry.logicalBytes == static_cast<quint64>(payload.size())
        && entry.allocatedBytes == allocatedBytes(original), QStringLiteral("Native logical/allocated size disagrees with the indexed default stream"));
    quint64 countedLogical = 0, countedAllocated = 0;
    for (auto it = initial->entries.cbegin(); it != initial->entries.cend(); ++it)
        if (it.key() == reference) { countedLogical += it->logicalBytes; countedAllocated += it->allocatedBytes; }
    require(countedLogical == static_cast<quint64>(payload.size()) && countedAllocated == allocatedBytes(original),
        QStringLiteral("Space accounting counted a hard-linked stream more than once"));
    const QByteArray initialFingerprint = fingerprint(*initial);
    const std::array<quint64, 3> diagnosticReferences{reference, removedReference, duringScanReference};
    require(QFile::rename(original, renamed), QStringLiteral("Cannot rename the fixture file"));
    const QByteArray larger(payload.size() + 8192, 'R');
    writeFile(renamed, larger);
    require(fileReference(renamed) == reference, QStringLiteral("Rename/write changed the native file identity"));
    require(QFile::remove(removed), QStringLiteral("Cannot delete the fixture file"));
    const auto updated = NtfsFastIndex::refresh(initial);
    readyRefresh(updated, QStringLiteral("Native USN refresh failed"), root, initial->nextUsn, diagnosticReferences);
    require(updated->nextUsn > initial->nextUsn, QStringLiteral("USN refresh did not advance its captured cursor"));
    require(updated->entries.contains(reference) && !updated->entries.contains(removedReference), QStringLiteral("USN refresh lost the live file or retained the deleted identity"));
    const auto updatedPaths = aliases(*updated, updated->entries[reference]);
    require(containsPath(updatedPaths, renamed) && !containsPath(updatedPaths, original)
        && containsPath(updatedPaths, link) && containsPath(updatedPaths, secondLink), QStringLiteral("USN refresh retained an obsolete hard-link name"));
    require(updated->entries[reference].logicalBytes == static_cast<quint64>(larger.size())
        && updated->entries[reference].allocatedBytes == allocatedBytes(renamed), QStringLiteral("USN refresh did not re-read the real default-stream sizes"));
    require(fingerprint(*initial) == initialFingerprint && initial->complete,
        QStringLiteral("Refresh mutated the previously published snapshot"));
    const QByteArray updatedFingerprint = fingerprint(*updated);
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    const auto cancelledBuild = NtfsFastIndex::build(root, cancelled);
    require(cancelledBuild && cancelledBuild->cancelled && !cancelledBuild->complete,
        QStringLiteral("A pre-cancelled build published a usable snapshot"));
    const auto cancelledRefresh = NtfsFastIndex::refresh(updated, cancelled);
    const bool preCancelledRefreshPreserved = cancelledRefresh && cancelledRefresh->cancelled && !cancelledRefresh->complete
        && fingerprint(*cancelledRefresh) == updatedFingerprint;
    if (!preCancelledRefreshPreserved) diagnoseRefresh(root, updated->nextUsn, diagnosticReferences);
    require(preCancelledRefreshPreserved, QStringLiteral("A pre-cancelled refresh discarded or changed the previous snapshot data"));
    cancelled->store(false);
    int buildProgress = 0;
    const auto interruptedBuild = NtfsFastIndex::build(root, cancelled, [&](quint64, quint64) { ++buildProgress; cancelled->store(true); });
    require(buildProgress > 0 && interruptedBuild && interruptedBuild->cancelled && !interruptedBuild->complete,
        QStringLiteral("Progress cancellation did not stop a native build"));
    writeFile(renamed, QByteArray(larger.size() + 4096, 'C'));
    cancelled->store(false);
    int refreshProgress = 0;
    const auto interruptedRefresh = NtfsFastIndex::refresh(updated, cancelled, [&](quint64, quint64) { ++refreshProgress; cancelled->store(true); });
    const bool interruptedRefreshPreserved = refreshProgress > 0 && interruptedRefresh && interruptedRefresh->cancelled && !interruptedRefresh->complete
        && fingerprint(*interruptedRefresh) == updatedFingerprint;
    if (!interruptedRefreshPreserved) diagnoseRefresh(root, updated->nextUsn, diagnosticReferences);
    require(interruptedRefreshPreserved, QStringLiteral("Progress cancellation published a partially applied journal"));
    require(fingerprint(*updated) == updatedFingerprint && updated->complete, QStringLiteral("Cancellation mutated the previous usable snapshot"));
    const auto final = NtfsFastIndex::refresh(updated);
    readyRefresh(final, QStringLiteral("Refresh after cancellation failed"), root, updated->nextUsn, diagnosticReferences);
    require(final->entries[reference].logicalBytes == static_cast<quint64>(larger.size() + 4096),
        QStringLiteral("Cancelled journal changes could not be replayed again"));
}
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(application.arguments().size() == 3, QStringLiteral("Expected dedicated volume root and existing work directory"));
        QString root = QDir::toNativeSeparators(application.arguments().at(1));
        require(root.size() == 3, QStringLiteral("Expected a dedicated drive root"));
        root[0] = root.at(0).toUpper();
        exercise(root, native(application.arguments().at(2)));
        std::printf("NATIVE_NTFS_INDEX_CHECKS=%d FAILURES=0 REAL_FSCTL=1 RAW_MFT=1 USN=1 SYSTEM_VOLUME=0\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NATIVE_NTFS_INDEX_FAIL checks=%d: %s\n", checks, error.what());
        return 1;
    }
}
