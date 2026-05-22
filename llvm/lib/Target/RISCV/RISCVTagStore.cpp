#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-tagstore"

namespace {

class RISCVTagStore : public MachineFunctionPass {
public:
  static char ID;

  RISCVTagStore() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "RISC-V TagStore Replacement Pass";
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
    const RISCVInstrInfo *TII = ST.getInstrInfo();

    bool Changed = false;

    for (MachineBasicBlock &MBB : MF) {
      for (auto I = MBB.begin(), E = MBB.end(); I != E;) {
        MachineInstr &MI = *I++;

        unsigned Opc = MI.getOpcode();

        if (Opc != RISCV::SH &&
            Opc != RISCV::SW)
          continue;

        DebugLoc DL = MI.getDebugLoc();

        MachineInstrBuilder MIB =
            BuildMI(MBB, MI, DL, TII->get(RISCV::SB));

        MIB.add(MI.getOperand(0)); // stored value
        MIB.add(MI.getOperand(1)); // base address
        MIB.add(MI.getOperand(2)); // immediate offset

        MIB.cloneMemRefs(MI);
        MIB->copyImplicitOps(MF, MI);
        MI.eraseFromParent();

        Changed = true;
      }
    }

    return Changed;
  }
};

} // end anonymous namespace

char RISCVTagStore::ID = 0;

INITIALIZE_PASS(RISCVTagStore, DEBUG_TYPE, "RISC-V TagStore Replacement Pass", false, false)

FunctionPass *llvm::createRISCVTagStorePass() {
  return new RISCVTagStore();
}