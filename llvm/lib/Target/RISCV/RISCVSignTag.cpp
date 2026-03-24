//===- RISCVSignTag.cpp - Replace instructions with their tagged version --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This pass rewrites instructions to their tagged version
//
//===---------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
// #define DEBUG_TYPE "riscv-tagged-collapse"
// #define RISCV_TAGGED_NAME "RISC-V Tagged definitions"

STATISTIC(NumDeadDefsReplaced, "Number of dead definitions replaced");

namespace {
enum class TagSignedNess : uint8_t {
  Unknown = 0,
  Signed = 1,
  Unsigned = 2,
};

struct RegTagState {
  TagSignedNess SignedNess = TagSignedNess::Unknown;
  unsigned WidthBits = 0;
  bool Valid = false;
};

static bool isRISCVSignedLoad(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case RISCV::LB:
  case RISCV::LH:
  case RISCV::LW:
    return true;
  default:
    return false;
  }
}

static bool isRISCVUnsignedLoad(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case RISCV::LBU:
  case RISCV::LHU:
  case RISCV::LWU:
    return true;
  default:
    return false;
  }
}

static unsigned getLoadWidthBits(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case RISCV::LB:
  case RISCV::LBU:
    return 8;
  case RISCV::LH:
  case RISCV::LHU:
    return 16;
  case RISCV::LW:
  case RISCV::LWU:
    return 32;
  default:
    return 0;
  }
}

static TagSignedNess classifyLoad(const MachineInstr &MI, unsigned &WidthBits) {
  WidthBits = getLoadWidthBits(MI);
  if (WidthBits == 0)
    return TagSignedNess::Unknown;
  if (isRISCVSignedLoad(MI))
    return TagSignedNess::Signed;
  if (isRISCVUnsignedLoad(MI))
    return TagSignedNess::Unsigned;
  return TagSignedNess::Unknown;
}

static TagSignedNess classifyUseContext(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case RISCV::SLTU:
  case RISCV::SLTIU:
    return TagSignedNess::Unsigned;
  case RISCV::SLT:
  case RISCV::SLTI:
    return TagSignedNess::Signed;
  default:
    return TagSignedNess::Unknown;
  }
}

static unsigned pickChangeTagPseudo(TagSignedness Desired, unsigned WidthBits) {
  if (Desired == TagSignedness::Unsigned) {
    if (WidthBits == 8)
      return RISCV::PSEUDO_CTUB;
    if (WidthBits == 16)
      return RISCV::PSEUDO_CTUH;
    return RISCV::PSEUDO_CTUW;
  }
  if (Desired == TagSignedness::Signed) {
    if (WidthBits == 8)
      return RISCV::PSEUDO_CTSB;
    if (WidthBits == 16)
      return RISCV::PSEUDO_CTSH;
    return RISCV::PSEUDO_CTSW;
  }
  return 0;
}

static void insertChangeTagBefore(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator InsertPt,
                                  const RISCVInstrInfo &TII, Register Reg,
                                  unsigned PseudoOpc) {
  DebugLoc DL;
  if (InsertPt != MBB.end())
    DL = InsertPt->getDebugLoc();

  auto MIB = BuildMI(MBB, InsertPt, DL, TII.get(PseudoOpc));
  // rd = rs1 (same virtual register, modeling in-place retag)
  MIB.addReg(Reg, RegState::Define);
  MIB.addReg(Reg);
}

class RISCVSignTagged : public MachineFunctionPass {
public:
  static char ID;

  RISCVSignTagged() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override;
};

bool RISCVSignTagged::runOnMachineFunction(MachineFunction &MF) override {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  if (!TII)
    return false;

  bool MadeChange = false;
  DenseMap<Register, RegTagState> RegTagMap;

  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      unsigned WidthBits = 0;
      TagSignedNess LoadS = classifyLoad(MI, WidthBits);
      if (LoadS != TagSignedNess::Unknown) {
        if (MI.getNumOperands() > 0 && MI.getOperand(0).isReg()) {
          Register DstReg = MI.getOperand(0).getReg();
          if (DstReg.isValid()) {
            RegTagState &S = RegTagMap[DstReg];
            S.SignedNess = LoadS;
            S.WidthBits = WidthBits;
            S.Valid = true;
          }
        }
        continue;
      }

      TagSignedNess UseS = classifyUseContext(MI);
      if (UseS == TagSignedNess::Unknown)
        continue;

      for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || !MO.isUse())
          continue;

        Register R = MO.getReg();
        if (!R.isValid())
          continue;

        auto It = RegTagMap.find(R);
        if (It == RegTagMap.end() || !It->second.Valid)
          continue;

        RegTagState &S = It->second;
        if (S.SignedNess == TagSignedNess::Unknown)
          continue;

        if (S.SignedNess != UseS) {
          unsigned PseudoOpcode = pickChangeTagPseudo(UseS, S.WidthBits);
          if (!PseudoOpcode)
            continue;

          insertChangeTagBefore(MBB, MI.getIterator(), *TII, R, PseudoOpcode);
          MadeChange = true;

          S.SignedNess = UseS;
        }
      }
    }
  }

  return MadeChange;
}

StringRef RISCVSignTagged::getPassName() const override {
  return "RISC-V Fix Load Tag Mismatch (per-register, pre-regalloc)";
}
} // end anonymous namespace

char RISCVSignTagged::ID = 0;

INITIALIZE_PASS(RISCVSignTagged, DEBUG_TYPE,
                "RISC-V Fix Load Tag Mismatch (per-register, pre-regalloc)",
                false, false)

FunctionPass *llvm::createRISCVSignTaggedPass() {
  return new RISCVSignTagged();
}