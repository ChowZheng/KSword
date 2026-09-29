#pragma once
#include "ArkDriverClient.h"
#include "../../../shared/driver/KswordArkPfnIoctl.h"

namespace ksword::ark {
// Native query first; the driver supplies the same bounded identity ABI when
// Windows denies the user-mode query. A thread-local token is restored on exit.
class PfnQueryClient final {
public:
    PfnQueryClient();
    ~PfnQueryClient();
    PfnQueryClient(const PfnQueryClient&) = delete;
    PfnQueryClient& operator=(const PfnQueryClient&) = delete;
    long ranges(std::vector<KSWORD_ARK_PFN_RANGE>& result);
    long owners(std::vector<KSWORD_ARK_PFN_OWNER>& result);
    long pages(std::uint64_t first, std::uint32_t count, std::vector<KSWORD_ARK_PFN_IDENTITY>& result);
    long identities(const std::vector<std::uint64_t>& pfns, std::vector<KSWORD_ARK_PFN_IDENTITY>& result);
    bool mappingAvailable();
    long mappings(std::uint32_t pid, std::uint64_t created, const std::vector<std::uint64_t>& addresses,
        std::vector<KSWORD_ARK_PFN_MAPPING>& result);
    std::uint64_t nativeBatches = 0;
    std::uint64_t driverBatches = 0;
private:
    long nativeQuery(unsigned long kind, void* buffer, unsigned long bytes) const;
    long driverQuery(unsigned long kind, std::uint64_t first, std::uint32_t count,
        void* records, unsigned long capacity, unsigned long stride, unsigned long& returned,
        const std::vector<std::uint64_t>* pfns = nullptr);
    DriverClient m_client;
    DriverHandle m_driver;
    HANDLE m_previousToken = nullptr;
    bool m_impersonating = false;
    bool m_driverPages = false;
};
}
