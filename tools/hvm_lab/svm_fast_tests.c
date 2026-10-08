#include <stdio.h>
#include <string.h>
#include "../../KswordARKDriver/src/features/hvm/hvm_svm_fast.h"
#include "../../KswordARKDriver/src/features/hvm/hvm_svm_nested_register.h"

static KSW_SVM_FAST fast;
static KSW_SVM_PERF perf;
static KSWORD_HVM_HOTSPOTS hot;
static KSW_SVM_VMCB current;
static KSW_SVM_U64 gpr[16], counters[9], virtualEfer;
static unsigned machineAction, checks;
static KSW_NSVM_SESSION session;
static KSW_NSVM_EXECUTION execution;
static KSW_NSVM_MACHINE machine;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL line=%u\n", __LINE__); return 1; } } while (0)

static void initialize(unsigned slot, unsigned write)
{
    static const unsigned msrs[] = {0xc0000080U,0xc0010117U,0xda0U};
    memset(&fast,0,sizeof(fast)); memset(&perf,0,sizeof(perf)); memset(&hot,0,sizeof(hot));
    memset(&current,0,sizeof(current)); memset(gpr,0,sizeof(gpr)); memset(counters,0,sizeof(counters));
    fast.Enabled = 1; fast.Efer = virtualEfer = 0x4d01; fast.Hsave = 0x12345000;
    fast.Xss = 0x800; fast.XsaveFeatures = 8; fast.PhysicalTpr = 3; fast.IntCtl = 3;
    fast.VirtualEfer = &virtualEfer; fast.Hot = &hot; fast.Perf = &perf;
    fast.HotSequence = &counters[0]; fast.GeneralSequence = &counters[1];
    fast.GeneralExits = &counters[2]; fast.GeneralLastExit = &counters[3];
    fast.VmExits = &counters[4]; fast.RawExit = &counters[5]; fast.LegacyTlb = &counters[6];
    fast.Transitions = &counters[7]; fast.MachineLastExit = &counters[8]; fast.MachineLastAction = &machineAction;
    hot.levels[0].msrUsed = 3;
    hot.levels[0].msrs[0].number = msrs[0]; hot.levels[0].msrs[1].number = msrs[1]; hot.levels[0].msrs[2].number = msrs[2];
    KswSvmWrite64(&current,KSW_VMCB_EXITCODE,0x7c); KswSvmWrite64(&current,KSW_VMCB_EXITINFO1,write);
    KswSvmWrite64(&current,KSW_VMCB_INTCTL,3); KswSvmWrite64(&current,KSW_VMCB_CR0,0x80000001);
    KswSvmWrite64(&current,KSW_VMCB_EFER,0x4d01); KswSvmWrite64(&current,KSW_VMCB_RIP,0x1000);
    KswSvmWrite64(&current,KSW_VMCB_NRIP,0x1002); gpr[1] = msrs[slot];
    if(write){KswSvmWrite64(&current,KSW_VMCB_RAX,slot==0?fast.Efer:slot==1?fast.Hsave:fast.Xss);}
}

static int test_equivalence(void)
{
    unsigned slot, write, selected;
    for(slot=0;slot<3;++slot){for(write=0;write<2;++write){for(selected=0;selected<2;++selected){
        KSW_NSVM_REGISTER_IO io={0}; KSW_NSVM_MSRS svm={0}; KSW_SVM_VMCB expected;
        KSW_SVM_U64 xss=0x800, value;
        initialize(slot,write); expected=current; perf.Selected=selected; perf.Sequence=selected;
        io.Current=&expected; io.Svm=&svm; io.GuestXss=&xss; io.PreparedXss=0x800;
        io.XsaveFeatures=8; io.EferAllowed=0x5d01; io.ExposeSvm=1; svm.Efer=fast.Efer; svm.AddressMask=0x0000fffffffff000ULL;
        svm.Hsave=fast.Hsave; value=KswSvmRead64(&current,KSW_VMCB_RAX);
        CHECK(KswSvmNestedRegisterAccess(&io,(unsigned)gpr[1],write,&value)==KSW_NSVM_MSR_OK);
        CHECK(KswSvmFastTry(&fast,&current,gpr,3)==1);
        CHECK(KswSvmRead64(&current,KSW_VMCB_RIP)==0x1002);
        if(!write){CHECK(KswSvmRead64(&current,KSW_VMCB_RAX)==(value&0xffffffffULL));CHECK(gpr[2]==value>>32);}
        CHECK(hot.levels[0].total==1 && hot.levels[0].codes[0x7c]==1);
        CHECK(perf.Metrics.fastMsr[slot][write]==1 && perf.Metrics.tlbIssued[0]==1);
        CHECK(counters[0]==2 && counters[1]==2 && counters[2]==1 && counters[4]==1 && counters[7]==1);
        CHECK(perf.Sequence==(selected?1:2)); CHECK(perf.Row==(selected?&perf.Metrics.rows[0][0]:NULL));
    }}}
    initialize(0,0); KswSvmWrite64(&current,KSW_VMCB_EFER,0x4901);
    CHECK(KswSvmFastTry(&fast,&current,gpr,3)==1 && KswSvmRead64(&current,KSW_VMCB_RAX)==0x4901);
    return 0;
}

static int test_decline_no_mutation(void)
{
    unsigned scenario;
    for(scenario=0;scenario<13;++scenario){
        KSW_SVM_VMCB before; KSW_SVM_U64 saved[16];
        initialize(0,1);
        switch(scenario){
        case 0:fast.Enabled=0;break;
        case 1:fast.PhysicalTpr=4;break;
        case 2:KswSvmWrite64(&current,KSW_VMCB_INTCTL,4);break;
        case 3:KswSvmWrite64(&current,KSW_VMCB_EVENT,0x80000020);break;
        case 4:KswSvmWrite64(&current,KSW_VMCB_EXITINTINFO,0x80000020);break;
        case 5:((unsigned char*)&current)[KSW_VMCB_CPL]=3;break;
        case 6:KswSvmWrite64(&current,KSW_VMCB_NRIP,0x1000);break;
        case 7:KswSvmWrite64(&current,KSW_VMCB_EXITINFO1,2);break;
        case 8:gpr[1]=0x123;break;
        case 9:KswSvmWrite64(&current,KSW_VMCB_RAX,0x4d00);break;
        case 10:hot.levels[0].total=~0ULL;break;
        case 11:gpr[1]=0xda0;fast.XsaveFeatures=0;break;
        default:KswSvmWrite64(&current,KSW_VMCB_RFLAGS,1ULL<<17);break;
        }
        before=current;memcpy(saved,gpr,sizeof(saved));
        CHECK(KswSvmFastTry(&fast,&current,gpr,3)==0);
        CHECK(memcmp(&current,&before,sizeof(before))==0 && memcmp(gpr,saved,sizeof(saved))==0);
        CHECK(counters[0]==0 && counters[1]==0 && counters[2]==0 && perf.Sequence==0);
    }
    return 0;
}

static int test_entry_eligibility(void)
{
    unsigned scenario;
    for(scenario=0;scenario<20;++scenario){
        memset(&session,0,sizeof(session)); memset(&execution,0,sizeof(execution)); memset(&machine,0,sizeof(machine));
        initialize(0,0); execution.Current=&current; execution.Session=&session; execution.Gif=1;
        machine.Execution=&execution; machine.Overlay.Applied=1;
        CHECK(KswSvmFastEligible(&machine,0)==1 && KswSvmFastEligible(&machine,1)==0);
        execution.Gif=0; machine.Overlay.ForcedMask=1;
        CHECK(KswSvmFastEligible(&machine,0)==1);
        switch(scenario){
        case 0:session.Phase=1;break; case 1:session.Lease.Token=1;break;
        case 2:execution.Pending.Count=1;break; case 3:execution.RetryEventToken=1;break;
        case 4:machine.ArmedToken=1;break; case 5:machine.ArmedObservation=1;break;
        case 6:machine.PhysicalNmiToken=1;break; case 7:machine.HeldNmiGuard=1;break;
        case 8:machine.NmiCount=1;break; case 9:machine.NmiHardwareMask=1;break;
        case 10:machine.NmiBlocked=1;break; case 11:machine.IrqWindow.Applied=1;break;
        case 12:machine.NmiWindow.Applied=1;break; case 13:machine.Iret.Applied=1;break;
        case 14:machine.Iret.Requested=1;break; case 15:machine.Overlay.Applied=0;break;
        case 16:machine.Overlay.Inner=1;break; case 17:machine.Overlay.ForcedMask=0;break;
        case 18:KswSvmWrite64(&current,KSW_VMCB_EVENT,0x80000020);break;
        default:KswSvmWrite64(&current,KSW_VMCB_INTCTL,0x100);break;
        }
        CHECK(KswSvmFastEligible(&machine,0)==0);
    }
    return 0;
}
int main(void)
{
    if(test_equivalence() || test_decline_no_mutation() || test_entry_eligibility()){return 1;}
    printf("SVM_FAST_CHECKS=%u PASS (portable scalar policy; no hardware XSTATE proof)\n",checks);return 0;
}
