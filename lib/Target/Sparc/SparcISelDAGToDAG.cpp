//===-- SparcISelDAGToDAG.cpp - A dag to dag inst selector for Sparc ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the SPARC target.
//
//===----------------------------------------------------------------------===//

#include "SparcTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
/// SparcDAGToDAGISel - SPARC specific code to select SPARC machine
/// instructions for SelectionDAG operations.
///
namespace {
class SparcDAGToDAGISel : public SelectionDAGISel {
  /// Subtarget - Keep a pointer to the Sparc Subtarget around so that we can
  /// make the right decision when generating code for different targets.
  const SparcSubtarget *Subtarget;
public:
  explicit SparcDAGToDAGISel(SparcTargetMachine &tm) : SelectionDAGISel(tm) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    Subtarget = &MF.getSubtarget<SparcSubtarget>();
    return SelectionDAGISel::runOnMachineFunction(MF);
  }

  SDNode *Select(SDNode *N) override;

  // Complex Pattern Selectors.
  bool SelectADDRrr(SDValue N, SDValue &R1, SDValue &R2);
  bool SelectADDRri(SDValue N, SDValue &Base, SDValue &Offset);

  /// SelectInlineAsmMemoryOperand - Implement addressing mode selection for
  /// inline asm expressions.
  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    unsigned ConstraintID,
                                    std::vector<SDValue> &OutOps) override;

  const char *getPassName() const override {
    return "SPARC DAG->DAG Pattern Instruction Selection";
  }

  // Include the pieces autogenerated from the target description.
#include "SparcGenDAGISel.inc"

private:
  SDNode* getGlobalBaseReg();
};
}  // end anonymous namespace

SDNode* SparcDAGToDAGISel::getGlobalBaseReg() {
  unsigned GlobalBaseReg = Subtarget->getInstrInfo()->getGlobalBaseReg(MF);
  return CurDAG->getRegister(GlobalBaseReg, TLI->getPointerTy()).getNode();
}

bool SparcDAGToDAGISel::SelectADDRri(SDValue Addr,
                                     SDValue &Base, SDValue &Offset) {
  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), TLI->getPointerTy());
    Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), MVT::i32);
    return true;
  }
  if (Addr.getOpcode() == ISD::TargetExternalSymbol ||
      Addr.getOpcode() == ISD::TargetGlobalAddress ||
      Addr.getOpcode() == ISD::TargetGlobalTLSAddress)
    return false;  // direct calls.

  if (Addr.getOpcode() == ISD::ADD) {
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1))) {
      if (isInt<13>(CN->getSExtValue())) {
        if (FrameIndexSDNode *FIN =
                dyn_cast<FrameIndexSDNode>(Addr.getOperand(0))) {
          // Constant offset from frame ref.
          Base =
              CurDAG->getTargetFrameIndex(FIN->getIndex(), TLI->getPointerTy());
        } else {
          Base = Addr.getOperand(0);
        }
        Offset = CurDAG->getTargetConstant(CN->getZExtValue(), SDLoc(Addr),
                                           MVT::i32);
        return true;
      }
    }
    if (Addr.getOperand(0).getOpcode() == SPISD::Lo) {
      Base = Addr.getOperand(1);
      Offset = Addr.getOperand(0).getOperand(0);
      return true;
    }
    if (Addr.getOperand(1).getOpcode() == SPISD::Lo) {
      Base = Addr.getOperand(0);
      Offset = Addr.getOperand(1).getOperand(0);
      return true;
    }
  }
  Base = Addr;
  Offset = CurDAG->getTargetConstant(0, SDLoc(Addr), MVT::i32);
  return true;
}

bool SparcDAGToDAGISel::SelectADDRrr(SDValue Addr, SDValue &R1, SDValue &R2) {
  if (Addr.getOpcode() == ISD::FrameIndex) return false;
  if (Addr.getOpcode() == ISD::TargetExternalSymbol ||
      Addr.getOpcode() == ISD::TargetGlobalAddress ||
      Addr.getOpcode() == ISD::TargetGlobalTLSAddress)
    return false;  // direct calls.

  if (Addr.getOpcode() == ISD::ADD) {
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1)))
      if (isInt<13>(CN->getSExtValue()))
        return false;  // Let the reg+imm pattern catch this!
    if (Addr.getOperand(0).getOpcode() == SPISD::Lo ||
        Addr.getOperand(1).getOpcode() == SPISD::Lo)
      return false;  // Let the reg+imm pattern catch this!
    R1 = Addr.getOperand(0);
    R2 = Addr.getOperand(1);
    return true;
  }

  R1 = Addr;
  R2 = CurDAG->getRegister(SP::G0, TLI->getPointerTy());
  return true;
}

SDNode *SparcDAGToDAGISel::Select(SDNode *N) {
  SDLoc dl(N);
  if (N->isMachineOpcode()) {
    N->setNodeId(-1);
    return nullptr;   // Already selected.
  }

  switch (N->getOpcode()) {
  default: break;
  case SPISD::GLOBAL_BASE_REG:
    return getGlobalBaseReg();

  case ISD::SDIV:
  case ISD::UDIV: {
    // sdivx / udivx handle 64-bit divides.
    if (N->getValueType(0) == MVT::i64)
      break;
    // FIXME: should use a custom expander to expose the SRA to the dag.
    SDValue DivLHS = N->getOperand(0);
    SDValue DivRHS = N->getOperand(1);

    // Set the Y register to the high-part.
    SDValue TopPart;
    if (N->getOpcode() == ISD::SDIV) {
      TopPart = SDValue(CurDAG->getMachineNode(SP::SRAri, dl, MVT::i32, DivLHS,
                                   CurDAG->getTargetConstant(31, dl, MVT::i32)),
                        0);
    } else {
      TopPart = CurDAG->getRegister(SP::G0, MVT::i32);
    }
    TopPart = SDValue(CurDAG->getMachineNode(SP::WRASRrr, dl, MVT::i32,
                                 TopPart,
                                 CurDAG->getRegister(SP::G0, MVT::i32)), 0);
    TopPart = CurDAG->getCopyToReg(TopPart, dl, SP::Y, TopPart, SDValue()).getValue(1);

    // FIXME: Handle div by immediate.
    unsigned Opcode = N->getOpcode() == ISD::SDIV ? SP::SDIVrr : SP::UDIVrr;
    return CurDAG->SelectNodeTo(N, Opcode, MVT::i32, DivLHS, DivRHS,
                                TopPart);
  }
  case ISD::MULHU:
  case ISD::MULHS: {
    // FIXME: Handle mul by immediate.
    SDValue MulLHS = N->getOperand(0);
    SDValue MulRHS = N->getOperand(1);
    unsigned Opcode = N->getOpcode() == ISD::MULHU ? SP::UMULrr : SP::SMULrr;
    SDNode *Mul = CurDAG->getMachineNode(Opcode, dl, MVT::i32, MVT::Glue,
                                         MulLHS, MulRHS);
    // The high part is in the Y register.
    return CurDAG->SelectNodeTo(N, SP::RDASR, MVT::i32,
                                CurDAG->getRegister(SP::Y, MVT::i32),
                                SDValue(Mul, 1));
  }
  }

  return SelectCode(N);
}


/// SelectInlineAsmMemoryOperand - Implement addressing mode selection for
/// inline asm expressions.
bool
SparcDAGToDAGISel::SelectInlineAsmMemoryOperand(const SDValue &Op,
                                                unsigned ConstraintID,
                                                std::vector<SDValue> &OutOps) {
  SDValue Op0, Op1;
  switch (ConstraintID) {
  default: return true;
  case InlineAsm::Constraint_i:
  case InlineAsm::Constraint_m: // memory
   if (!SelectADDRrr(Op, Op0, Op1))
     SelectADDRri(Op, Op0, Op1);
   break;
  }

  OutOps.push_back(Op0);
  OutOps.push_back(Op1);
  return false;
}

/// createSparcISelDag - This pass converts a legalized DAG into a
/// SPARC-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createSparcISelDag(SparcTargetMachine &TM) {
  return new SparcDAGToDAGISel(TM);
}
