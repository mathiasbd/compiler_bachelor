#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

#define PASS_NAME "RISC-V Replace Stores with Tagged Store Instruction"

namespace {
class RISCVReplaceStores : public MachineFunctionPass {
public:
  static char ID;
  RISCVReplaceStores() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_NAME; }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  static bool shouldReplaceStore(const MachineInstr &MI);
};
} // namespace

char RISCVReplaceStores::ID = 0;

INITIALIZE_PASS(RISCVReplaceStores, "riscv-replace-stores-with-st", PASS_NAME,
                false, false)

FunctionPass *llvm::createRISCVReplaceStoresPass() {
  return new RISCVReplaceStores();
}

bool RISCVReplaceStores::runOnMachineFunction(MachineFunction &MF) {
  const auto *TII =
      static_cast<const RISCVInstrInfo *>(MF.getSubtarget().getInstrInfo());

  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    for (auto I = MBB.begin(), E = MBB.end(); I != E;) {
      MachineInstr &MI = *I++;

      if (!shouldReplaceStore(MI)) {
        continue;
      }

      MachineInstrBuilder MIB =
          BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(RISCV::ST));

      for (const MachineOperand &MO : MI.operands()) {
        MIB.add(MO);
      }

      MIB->cloneMemRefs(MF, MI);
      MI.eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}

bool RISCVReplaceStores::shouldReplaceStore(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case RISCV::SB:
  case RISCV::SH:
  case RISCV::SW:
    return true;
  default:
    return false;
  }
}
