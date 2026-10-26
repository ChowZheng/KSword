#pragma once

#include "../../../shared/usermode/backend/security/CodeIntegrity.h"

#include <vector>

namespace Ksword::Features::Misc {

// MiscAuditPageId 标识一个只读审计标签页。六个页面共用同一套列，但数据来源
// 完全不同，所以采集器必须按 id 分派而不是让视图自己拼。
enum class MiscAuditPageId {
    CodeIntegrity,
    VbsHvciSkci,
    HyperV,
    AppLocker,
    BamAhcache,
    Bugcheck,
};

// CollectRowsForPage 采集一个标签页的完整只读审计行。
// 输入：稳定的页面 id；处理：只跑 R3 查询/注册表读取/服务查询与 ArkDriverClient
// 只读 wrapper；输出：可直接铺进列表的新鲜行快照。
std::vector<ks::r3::security::MiscAuditRow> CollectRowsForPage(MiscAuditPageId pageId);

} // namespace Ksword::Features::Misc
