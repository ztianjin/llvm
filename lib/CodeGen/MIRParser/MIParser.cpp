//===- MIParser.cpp - Machine instructions parser implementation ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the parsing of machine instructions.
//
//===----------------------------------------------------------------------===//

#include "MIParser.h"
#include "MILexer.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/AsmParser/SlotMapping.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Target/TargetInstrInfo.h"

using namespace llvm;

namespace {

class MIParser {
  SourceMgr &SM;
  MachineFunction &MF;
  SMDiagnostic &Error;
  StringRef Source, CurrentSource;
  MIToken Token;
  /// Maps from basic block numbers to MBBs.
  const DenseMap<unsigned, MachineBasicBlock *> &MBBSlots;
  /// Maps from indices to unnamed global values and metadata nodes.
  const SlotMapping &IRSlots;
  /// Maps from instruction names to op codes.
  StringMap<unsigned> Names2InstrOpCodes;
  /// Maps from register names to registers.
  StringMap<unsigned> Names2Regs;

public:
  MIParser(SourceMgr &SM, MachineFunction &MF, SMDiagnostic &Error,
           StringRef Source,
           const DenseMap<unsigned, MachineBasicBlock *> &MBBSlots,
           const SlotMapping &IRSlots);

  void lex();

  /// Report an error at the current location with the given message.
  ///
  /// This function always return true.
  bool error(const Twine &Msg);

  /// Report an error at the given location with the given message.
  ///
  /// This function always return true.
  bool error(StringRef::iterator Loc, const Twine &Msg);

  MachineInstr *parse();

  bool parseRegister(unsigned &Reg);
  bool parseRegisterOperand(MachineOperand &Dest, bool IsDef = false);
  bool parseImmediateOperand(MachineOperand &Dest);
  bool parseMBBOperand(MachineOperand &Dest);
  bool parseGlobalAddressOperand(MachineOperand &Dest);
  bool parseMachineOperand(MachineOperand &Dest);

private:
  /// Convert the integer literal in the current token into an unsigned integer.
  ///
  /// Return true if an error occurred.
  bool getUnsigned(unsigned &Result);

  void initNames2InstrOpCodes();

  /// Try to convert an instruction name to an opcode. Return true if the
  /// instruction name is invalid.
  bool parseInstrName(StringRef InstrName, unsigned &OpCode);

  bool parseInstruction(unsigned &OpCode);

  void initNames2Regs();

  /// Try to convert a register name to a register number. Return true if the
  /// register name is invalid.
  bool getRegisterByName(StringRef RegName, unsigned &Reg);
};

} // end anonymous namespace

MIParser::MIParser(SourceMgr &SM, MachineFunction &MF, SMDiagnostic &Error,
                   StringRef Source,
                   const DenseMap<unsigned, MachineBasicBlock *> &MBBSlots,
                   const SlotMapping &IRSlots)
    : SM(SM), MF(MF), Error(Error), Source(Source), CurrentSource(Source),
      Token(MIToken::Error, StringRef()), MBBSlots(MBBSlots), IRSlots(IRSlots) {
}

void MIParser::lex() {
  CurrentSource = lexMIToken(
      CurrentSource, Token,
      [this](StringRef::iterator Loc, const Twine &Msg) { error(Loc, Msg); });
}

bool MIParser::error(const Twine &Msg) { return error(Token.location(), Msg); }

bool MIParser::error(StringRef::iterator Loc, const Twine &Msg) {
  // TODO: Get the proper location in the MIR file, not just a location inside
  // the string.
  assert(Loc >= Source.data() && Loc <= (Source.data() + Source.size()));
  Error = SMDiagnostic(
      SM, SMLoc(),
      SM.getMemoryBuffer(SM.getMainFileID())->getBufferIdentifier(), 1,
      Loc - Source.data(), SourceMgr::DK_Error, Msg.str(), Source, None, None);
  return true;
}

MachineInstr *MIParser::parse() {
  lex();

  // Parse any register operands before '='
  // TODO: Allow parsing of multiple operands before '='
  MachineOperand MO = MachineOperand::CreateImm(0);
  SmallVector<MachineOperand, 8> Operands;
  if (Token.isRegister()) {
    if (parseRegisterOperand(MO, /*IsDef=*/true))
      return nullptr;
    Operands.push_back(MO);
    if (Token.isNot(MIToken::equal)) {
      error("expected '='");
      return nullptr;
    }
    lex();
  }

  unsigned OpCode;
  if (Token.isError() || parseInstruction(OpCode))
    return nullptr;

  // TODO: Parse the instruction flags and memory operands.

  // Parse the remaining machine operands.
  while (Token.isNot(MIToken::Eof)) {
    if (parseMachineOperand(MO))
      return nullptr;
    Operands.push_back(MO);
    if (Token.is(MIToken::Eof))
      break;
    if (Token.isNot(MIToken::comma)) {
      error("expected ',' before the next machine operand");
      return nullptr;
    }
    lex();
  }

  const auto &MCID = MF.getSubtarget().getInstrInfo()->get(OpCode);

  // Verify machine operands.
  if (!MCID.isVariadic()) {
    for (size_t I = 0, E = Operands.size(); I < E; ++I) {
      if (I < MCID.getNumOperands())
        continue;
      // Mark this register as implicit to prevent an assertion when it's added
      // to an instruction. This is a temporary workaround until the implicit
      // register flag can be parsed.
      Operands[I].setImplicit();
    }
  }

  // TODO: Determine the implicit behaviour when implicit register flags are
  // parsed.
  auto *MI = MF.CreateMachineInstr(MCID, DebugLoc(), /*NoImplicit=*/true);
  for (const auto &Operand : Operands)
    MI->addOperand(MF, Operand);
  return MI;
}

bool MIParser::parseInstruction(unsigned &OpCode) {
  if (Token.isNot(MIToken::Identifier))
    return error("expected a machine instruction");
  StringRef InstrName = Token.stringValue();
  if (parseInstrName(InstrName, OpCode))
    return error(Twine("unknown machine instruction name '") + InstrName + "'");
  lex();
  return false;
}

bool MIParser::parseRegister(unsigned &Reg) {
  switch (Token.kind()) {
  case MIToken::underscore:
    Reg = 0;
    break;
  case MIToken::NamedRegister: {
    StringRef Name = Token.stringValue();
    if (getRegisterByName(Name, Reg))
      return error(Twine("unknown register name '") + Name + "'");
    break;
  }
  // TODO: Parse other register kinds.
  default:
    llvm_unreachable("The current token should be a register");
  }
  return false;
}

bool MIParser::parseRegisterOperand(MachineOperand &Dest, bool IsDef) {
  unsigned Reg;
  // TODO: Parse register flags.
  if (parseRegister(Reg))
    return true;
  lex();
  // TODO: Parse subregister.
  Dest = MachineOperand::CreateReg(Reg, IsDef);
  return false;
}

bool MIParser::parseImmediateOperand(MachineOperand &Dest) {
  assert(Token.is(MIToken::IntegerLiteral));
  const APSInt &Int = Token.integerValue();
  if (Int.getMinSignedBits() > 64)
    // TODO: Replace this with an error when we can parse CIMM Machine Operands.
    llvm_unreachable("Can't parse large integer literals yet!");
  Dest = MachineOperand::CreateImm(Int.getExtValue());
  lex();
  return false;
}

bool MIParser::getUnsigned(unsigned &Result) {
  assert(Token.hasIntegerValue() && "Expected a token with an integer value");
  const uint64_t Limit = uint64_t(std::numeric_limits<unsigned>::max()) + 1;
  uint64_t Val64 = Token.integerValue().getLimitedValue(Limit);
  if (Val64 == Limit)
    return error("expected 32-bit integer (too large)");
  Result = Val64;
  return false;
}

bool MIParser::parseMBBOperand(MachineOperand &Dest) {
  assert(Token.is(MIToken::MachineBasicBlock));
  unsigned Number;
  if (getUnsigned(Number))
    return true;
  auto MBBInfo = MBBSlots.find(Number);
  if (MBBInfo == MBBSlots.end())
    return error(Twine("use of undefined machine basic block #") +
                 Twine(Number));
  MachineBasicBlock *MBB = MBBInfo->second;
  if (!Token.stringValue().empty() && Token.stringValue() != MBB->getName())
    return error(Twine("the name of machine basic block #") + Twine(Number) +
                 " isn't '" + Token.stringValue() + "'");
  Dest = MachineOperand::CreateMBB(MBB);
  lex();
  return false;
}

bool MIParser::parseGlobalAddressOperand(MachineOperand &Dest) {
  switch (Token.kind()) {
  case MIToken::NamedGlobalValue: {
    auto Name = Token.stringValue();
    const Module *M = MF.getFunction()->getParent();
    if (const auto *GV = M->getNamedValue(Name)) {
      Dest = MachineOperand::CreateGA(GV, /*Offset=*/0);
      break;
    }
    return error(Twine("use of undefined global value '@") + Name + "'");
  }
  case MIToken::GlobalValue: {
    unsigned GVIdx;
    if (getUnsigned(GVIdx))
      return true;
    if (GVIdx >= IRSlots.GlobalValues.size())
      return error(Twine("use of undefined global value '@") + Twine(GVIdx) +
                   "'");
    Dest = MachineOperand::CreateGA(IRSlots.GlobalValues[GVIdx],
                                    /*Offset=*/0);
    break;
  }
  default:
    llvm_unreachable("The current token should be a global value");
  }
  // TODO: Parse offset and target flags.
  lex();
  return false;
}

bool MIParser::parseMachineOperand(MachineOperand &Dest) {
  switch (Token.kind()) {
  case MIToken::underscore:
  case MIToken::NamedRegister:
    return parseRegisterOperand(Dest);
  case MIToken::IntegerLiteral:
    return parseImmediateOperand(Dest);
  case MIToken::MachineBasicBlock:
    return parseMBBOperand(Dest);
  case MIToken::GlobalValue:
  case MIToken::NamedGlobalValue:
    return parseGlobalAddressOperand(Dest);
  case MIToken::Error:
    return true;
  default:
    // TODO: parse the other machine operands.
    return error("expected a machine operand");
  }
  return false;
}

void MIParser::initNames2InstrOpCodes() {
  if (!Names2InstrOpCodes.empty())
    return;
  const auto *TII = MF.getSubtarget().getInstrInfo();
  assert(TII && "Expected target instruction info");
  for (unsigned I = 0, E = TII->getNumOpcodes(); I < E; ++I)
    Names2InstrOpCodes.insert(std::make_pair(StringRef(TII->getName(I)), I));
}

bool MIParser::parseInstrName(StringRef InstrName, unsigned &OpCode) {
  initNames2InstrOpCodes();
  auto InstrInfo = Names2InstrOpCodes.find(InstrName);
  if (InstrInfo == Names2InstrOpCodes.end())
    return true;
  OpCode = InstrInfo->getValue();
  return false;
}

void MIParser::initNames2Regs() {
  if (!Names2Regs.empty())
    return;
  // The '%noreg' register is the register 0.
  Names2Regs.insert(std::make_pair("noreg", 0));
  const auto *TRI = MF.getSubtarget().getRegisterInfo();
  assert(TRI && "Expected target register info");
  for (unsigned I = 0, E = TRI->getNumRegs(); I < E; ++I) {
    bool WasInserted =
        Names2Regs.insert(std::make_pair(StringRef(TRI->getName(I)).lower(), I))
            .second;
    (void)WasInserted;
    assert(WasInserted && "Expected registers to be unique case-insensitively");
  }
}

bool MIParser::getRegisterByName(StringRef RegName, unsigned &Reg) {
  initNames2Regs();
  auto RegInfo = Names2Regs.find(RegName);
  if (RegInfo == Names2Regs.end())
    return true;
  Reg = RegInfo->getValue();
  return false;
}

MachineInstr *
llvm::parseMachineInstr(SourceMgr &SM, MachineFunction &MF, StringRef Src,
                        const DenseMap<unsigned, MachineBasicBlock *> &MBBSlots,
                        const SlotMapping &IRSlots, SMDiagnostic &Error) {
  return MIParser(SM, MF, Error, Src, MBBSlots, IRSlots).parse();
}
