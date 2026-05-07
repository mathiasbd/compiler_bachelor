#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-tagged-collapse"
#define RISCV_TAGGED_NAME "RISC-V Tagged definitions"

namespace {

enum class ExtKind {
  Signed,
  Unsigned,
  Unknown
};

enum class ExtSize {
  Byte,
  Halfword,
  Word
};

class RISCVTagged : public MachineFunctionPass {
public:
  static char ID;
  RISCVTagged() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override { return RISCV_TAGGED_NAME; }

private:
  std::pair<ExtKind, ExtSize>
  classifyDef(Register R, const MachineRegisterInfo &MRI) const;
};

} // end anonymous namespace

char RISCVTagged::ID = 0;

INITIALIZE_PASS(RISCVTagged, DEBUG_TYPE, RISCV_TAGGED_NAME, false, false)

FunctionPass *llvm::createRISCVTaggedPass() { return new RISCVTagged(); }

std::pair<ExtKind, ExtSize>
RISCVTagged::classifyDef(Register R, const MachineRegisterInfo &MRI) const {
  if (!R.isVirtual())
    return {ExtKind::Unknown, ExtSize::Word};

  MachineInstr *Def = MRI.getUniqueVRegDef(R);
  if (!Def)
    return {ExtKind::Unknown, ExtSize::Word};

  switch (Def->getOpcode()) {
  case RISCV::LB:
    return {ExtKind::Signed, ExtSize::Byte};
  case RISCV::LH:
    return {ExtKind::Signed, ExtSize::Halfword};
  case RISCV::LW:
    return {ExtKind::Signed, ExtSize::Word};

  case RISCV::LBU:
    return {ExtKind::Unsigned, ExtSize::Byte};
  case RISCV::LHU:
    return {ExtKind::Unsigned, ExtSize::Halfword};
  case RISCV::LWU:
    return {ExtKind::Unsigned, ExtSize::Word};
  case RISCV::PSEUDO_CTUW_EXT:
  case RISCV::PSEUDO_CTUW_SAFETY:
    return {ExtKind::Unsigned, ExtSize::Word};

  case RISCV::PSEUDO_CTSW_EXT:
  case RISCV::PSEUDO_CTSW_SAFETY:
    return {ExtKind::Signed, ExtSize::Word};

  case TargetOpcode::COPY:
    if (Def->getOperand(1).isReg())
      return classifyDef(Def->getOperand(1).getReg(), MRI);
    return {ExtKind::Unknown, ExtSize::Word};

  default:
    if (Def->getOperand(1).isReg())
      return classifyDef(Def->getOperand(1).getReg(), MRI);
    return {ExtKind::Unknown, ExtSize::Word};
  }
}

static unsigned getInstruction(std::pair<ExtKind, ExtSize> K) {
  if (K.first == ExtKind::Signed) {
    switch (K.second) {
    case ExtSize::Byte:     return RISCV::PSEUDO_CTSB_SAFETY;
    case ExtSize::Halfword: return RISCV::PSEUDO_CTSH_SAFETY;
    case ExtSize::Word:     return RISCV::PSEUDO_CTSW_SAFETY;
    }
  } else if (K.first == ExtKind::Unsigned) {
    switch (K.second) {
    case ExtSize::Byte:     return RISCV::PSEUDO_CTUB_SAFETY;
    case ExtSize::Halfword: return RISCV::PSEUDO_CTUH_SAFETY;
    case ExtSize::Word:     return RISCV::PSEUDO_CTUW_SAFETY;
    }
  }
  llvm_unreachable("Unhandled ExtKind/ExtSize");
}

bool RISCVTagged::runOnMachineFunction(MachineFunction &MF) {
  //dbgs() << "Called the RISCVTagged machine function" << "\n";
  bool MadeChange = false;
  MachineRegisterInfo &MRI = MF.getRegInfo();
  //const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  
  for (MachineBasicBlock &MBB : MF) {
    for (auto MII = MBB.begin(), MIE = MBB.end(); MII != MIE; ) {
      MachineInstr &MI = *MII++;
      
      switch (MI.getOpcode()) {
      case RISCV::PSEUDO_CTUW_SAFETY:
      case RISCV::PSEUDO_CTSW_SAFETY: {
        //dbgs() << "Found a safety ct \n";
        if (!MI.getOperand(1).isReg())
          break;

        Register Src = MI.getOperand(1).getReg();
        auto K = classifyDef(Src, MRI);
        if (K.first != ExtKind::Unknown) {
          //dbgs() << "Inside first if statement \n";
          if((MI.getOpcode() == RISCV::PSEUDO_CTUW_SAFETY && K.first == ExtKind::Unsigned) || (MI.getOpcode() == RISCV::PSEUDO_CTSW_SAFETY && K.first == ExtKind::Signed)) {
            //dbgs() << "Inside the next \n";
            Register Dst = MI.getOperand(0).getReg();
            Register Src = MI.getOperand(1).getReg();

            if (Dst != Src)
              MRI.replaceRegWith(Dst, Src);
            
            if (Src.isVirtual())
              MRI.clearKillFlags(Src);
            MI.eraseFromParent();
            MadeChange = true;
            continue;
          }
          /*unsigned NewOpc = getInstruction(K);
          if (NewOpc != MI.getOpcode()) {
            MI.setDesc(TII->get(NewOpc));
            MadeChange = true;
          }*/
        }
        break;
      }
      default:
        break;
      }
    }
  }

  return MadeChange;
}