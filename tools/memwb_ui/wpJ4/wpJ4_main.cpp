// wpJ4_main.cpp
// 作用：WP-J4 离屏验证夹具的入口。依次跑五组测试，打印汇总行
// "wpJ4_tests: N checks, M failures"，以失败数为进程退出码。

#include "wpJ4_common.h"

#include <QApplication>

#include <iostream>

#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

// 提交前独立审核（S-wpJ4）追加：释放即填充 0xDD 的全局分配器，已并入本夹具。
// 作用：把"释放后使用"（例如遍历 std::map 时 erase 当前节点再自增迭代器）从
// "碰运气不崩"变成确定性读到 0xDD 垃圾指针而崩溃——此前 notifyWindowMayCover 的
// 快照拷贝（R9）被说成"MSVC Release 下不可确定性测试"，换成本分配器后原有
// 用例不改一行就确定性崩溃，所以这是对该类缺陷的牙齿。
void* operator new(std::size_t size)
{
    void* block = std::malloc(size != 0 ? size : 1);
    if (block == nullptr)
    {
        throw std::bad_alloc();
    }
    return block;
}

void* operator new[](std::size_t size)
{
    return operator new(size);
}

void operator delete(void* block) noexcept
{
    if (block != nullptr)
    {
        std::memset(block, 0xDD, _msize(block));
        std::free(block);
    }
}

void operator delete[](void* block) noexcept
{
    operator delete(block);
}

void operator delete(void* block, std::size_t) noexcept
{
    operator delete(block);
}

void operator delete[](void* block, std::size_t) noexcept
{
    operator delete(block);
}

int main(int argc, char** argv)
{
    // 需要 QApplication（不是 QCoreApplication）：真实 WorkbenchConfirmations.cpp
    // 用到 QtWidgets 的 QMessageBox 类型信息，哪怕测试全程注入假 IConfirmPrompter
    // 从不真的 exec() 一个对话框，QApplication 仍是 QtWidgets 类型系统运作所需的
    // 应用对象。QT_QPA_PLATFORM=offscreen 由构建脚本设置。
    QApplication app(argc, argv);

    wpj4_test::RunLifecycleTests();
    wpj4_test::RunCommitTests();
    wpj4_test::RunPendingStageTests();
    wpj4_test::RunUndoTests();
    wpj4_test::RunConfirmationTests();

    std::cout << "wpJ4_tests: " << wpj4_test::g_checks << " checks, "
              << wpj4_test::g_failures << " failures" << std::endl;
    return wpj4_test::g_failures;
}
