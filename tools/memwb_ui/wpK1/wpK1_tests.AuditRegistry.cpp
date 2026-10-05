// ============================================================
// wpK1_tests.AuditRegistry.cpp
// 作用：WorkbenchServicesAudit 里 AuditChainRegistry<Chain>（提交链路 -> 日志事件登记表）的
//       逐分支断言。
// 重点：
//   - 链路对象不要求可赋值（真实链路对象 kLogEvent 含 const 成员，既不可拷贝赋值也不可移动赋值；
//     下面的 NonAssignableChain 复刻这个形状，登记表若退回按值存放就会在这里编译失败）；
//   - 同键重入复用同一条链路并加深，关闭按层递减，减到零才真正移除；
//   - 有界：超过上限淘汰最早登记的链路。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesAudit.h"

#include <string>
#include <type_traits>

namespace svc = ksword::memwb_services_detail;

namespace
{
    // NonAssignableChain：复刻 kLogEvent 的形状——有 const 成员，可拷贝构造，不可赋值。
    struct NonAssignableChain
    {
        explicit NonAssignableChain(const int identityValue)
            : identity(identityValue)
        {
        }

        // identity：链路身份（相当于 kLogEvent 的 GUID）。const 使本类型不可赋值。
        const int identity;
    };

    static_assert(
        !std::is_copy_assignable_v<NonAssignableChain> && !std::is_move_assignable_v<NonAssignableChain>,
        "NonAssignableChain must stay non-assignable to reproduce the kLogEvent shape");

    // TestOpenFindClose：基本生命周期。
    void TestOpenFindClose()
    {
        svc::AuditChainRegistry<NonAssignableChain> registry(8U);
        WPK1_CHECK(registry.Size() == 0U);
        WPK1_CHECK(registry.Find("k1") == nullptr);

        // 打开：返回链路引用，Find 能取到同一对象。
        NonAssignableChain& opened = registry.Open("k1", NonAssignableChain(11));
        WPK1_CHECK(opened.identity == 11);
        WPK1_CHECK(registry.Size() == 1U);
        WPK1_CHECK(registry.Find("k1") == &opened);
        WPK1_CHECK(registry.DepthOf("k1") == 1U);

        // 不同键互不影响。
        NonAssignableChain& other = registry.Open("k2", NonAssignableChain(22));
        WPK1_CHECK(other.identity == 22);
        WPK1_CHECK(registry.Size() == 2U);
        WPK1_CHECK(registry.Find("k1")->identity == 11);
        WPK1_CHECK(registry.Find("k2")->identity == 22);

        // 关闭：深度 1 的链路被移除并返回 true；另一条不受影响。
        WPK1_CHECK(registry.Close("k1"));
        WPK1_CHECK(registry.Find("k1") == nullptr);
        WPK1_CHECK(registry.Size() == 1U);
        WPK1_CHECK(registry.Find("k2") != nullptr);

        // 关闭不存在的键：false，不改变登记表。
        WPK1_CHECK(!registry.Close("never-opened"));
        WPK1_CHECK(!registry.Close("k1"));
        WPK1_CHECK(registry.Size() == 1U);
        WPK1_CHECK(registry.DepthOf("never-opened") == 0U);
    }

    // TestNesting：同键重入——复用同一链路、加深、按层关闭。
    void TestNesting()
    {
        svc::AuditChainRegistry<NonAssignableChain> registry(8U);
        NonAssignableChain& outer = registry.Open("target", NonAssignableChain(1));
        // 重入：返回的是原链路（identity 仍是 1），传入的新对象被丢弃。
        NonAssignableChain& inner = registry.Open("target", NonAssignableChain(2));
        WPK1_CHECK(&inner == &outer);
        WPK1_CHECK(inner.identity == 1);
        WPK1_CHECK(registry.Size() == 1U);
        WPK1_CHECK(registry.DepthOf("target") == 2U);

        // 第一次关闭只减一层，链路仍在（内层 Busy 提交的结束不能拆掉外层链路）。
        WPK1_CHECK(!registry.Close("target"));
        WPK1_CHECK(registry.Find("target") == &outer);
        WPK1_CHECK(registry.DepthOf("target") == 1U);

        // 第二次关闭才真正移除。
        WPK1_CHECK(registry.Close("target"));
        WPK1_CHECK(registry.Find("target") == nullptr);
        WPK1_CHECK(registry.Size() == 0U);

        // 移除后重新打开得到新链路（identity 取新传入的值）。
        NonAssignableChain& reopened = registry.Open("target", NonAssignableChain(3));
        WPK1_CHECK(reopened.identity == 3);
        WPK1_CHECK(registry.DepthOf("target") == 1U);

        // 三层嵌套同理。
        registry.Open("target", NonAssignableChain(4));
        registry.Open("target", NonAssignableChain(5));
        WPK1_CHECK(registry.DepthOf("target") == 3U);
        WPK1_CHECK(!registry.Close("target"));
        WPK1_CHECK(!registry.Close("target"));
        WPK1_CHECK(registry.Close("target"));
        WPK1_CHECK(registry.Size() == 0U);
    }

    // TestBounded：有界与淘汰顺序。
    void TestBounded()
    {
        svc::AuditChainRegistry<NonAssignableChain> registry(3U);
        registry.Open("a", NonAssignableChain(1));
        registry.Open("b", NonAssignableChain(2));
        registry.Open("c", NonAssignableChain(3));
        WPK1_CHECK(registry.Size() == 3U);

        // 第四条：淘汰最早登记的 a，总数仍是 3。
        registry.Open("d", NonAssignableChain(4));
        WPK1_CHECK(registry.Size() == 3U);
        WPK1_CHECK(registry.Find("a") == nullptr);
        WPK1_CHECK(registry.Find("b") != nullptr);
        WPK1_CHECK(registry.Find("c") != nullptr);
        WPK1_CHECK(registry.Find("d") != nullptr);

        // 对已有键重入不占新名额，也不触发淘汰。
        registry.Open("b", NonAssignableChain(9));
        WPK1_CHECK(registry.Size() == 3U);
        WPK1_CHECK(registry.Find("c") != nullptr);
        WPK1_CHECK(registry.DepthOf("b") == 2U);

        // 淘汰按登记顺序持续进行：再开 e 淘汰 b（b 的深度随之丢失，是落单链路的兜底行为）。
        registry.Open("e", NonAssignableChain(5));
        WPK1_CHECK(registry.Size() == 3U);
        WPK1_CHECK(registry.Find("b") == nullptr);

        // 上限为 0 按 1 处理：永远只保留最新的一条。
        svc::AuditChainRegistry<NonAssignableChain> single(0U);
        single.Open("x", NonAssignableChain(1));
        single.Open("y", NonAssignableChain(2));
        WPK1_CHECK(single.Size() == 1U);
        WPK1_CHECK(single.Find("x") == nullptr);
        WPK1_CHECK(single.Find("y") != nullptr);

        // 大量不同键反复登记：登记表大小恒不超过上限（模拟落单链路泄漏的场景）。
        svc::AuditChainRegistry<int> leaky(64U);
        for (int index = 0; index < 5000; ++index)
        {
            leaky.Open("leak-" + std::to_string(index), index);
        }
        WPK1_CHECK(leaky.Size() == 64U);
        // 留下的是最新的 64 条。
        WPK1_CHECK(leaky.Find("leak-4999") != nullptr);
        WPK1_CHECK(leaky.Find("leak-4936") != nullptr);
        WPK1_CHECK(leaky.Find("leak-4935") == nullptr);
    }

    // TestSimpleChainType：用 int 当链路对象（真实代码之外的最简形态）也能工作，且 Find 返回的
    // 指针能被改写。
    void TestSimpleChainType()
    {
        svc::AuditChainRegistry<int> registry(4U);
        int& chain = registry.Open("k", 7);
        WPK1_CHECK(chain == 7);
        chain = 8;
        WPK1_CHECK(*registry.Find("k") == 8);
        WPK1_CHECK(registry.Close("k"));
    }
}

namespace wpK1_test
{
    void RunAuditRegistryTests()
    {
        TestOpenFindClose();
        TestNesting();
        TestBounded();
        TestSimpleChainType();
    }
}
