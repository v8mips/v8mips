// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/bits.h"
#include "src/compiler/instruction-selector-impl.h"
#include "src/compiler/node-matchers.h"

namespace v8 {
namespace internal {
namespace compiler {

#define TRACE_UNIMPL() PrintF("instruction_selector_mips: unimplemnted %s at line %d\n", \
    __FUNCTION__, __LINE__)


// Adds Mips-specific methods for generating InstructionOperands.
class MipsOperandGenerator V8_FINAL : public OperandGenerator {
 public:
  explicit MipsOperandGenerator(InstructionSelector* selector)
      : OperandGenerator(selector) {}

  InstructionOperand* UseOperand(Node* node, InstructionCode opcode) {
    if (CanBeImmediate(node, opcode)) {
      return UseImmediate(node);
    }
    return UseRegister(node);
  }

  bool CanBeImmediate(Node* node, InstructionCode opcode) {
    TRACE_UNIMPL();
    return false;
  }

 private:
  bool ImmediateFitsAddrMode1Instruction(int32_t imm) const {
    TRACE_UNIMPL();
    return false;
  }
};



void InstructionSelector::VisitLoad(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitStore(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32And(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Or(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Xor(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Shl(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Shr(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Sar(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Ror(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32Add(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32Sub(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32Mul(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32Div(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32UDiv(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32Mod(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32UMod(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitChangeInt32ToFloat64(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitChangeUint32ToFloat64(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitChangeFloat64ToInt32(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitChangeFloat64ToUint32(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitFloat64Add(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitFloat64Sub(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitFloat64Mul(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitFloat64Div(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitFloat64Mod(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitCall(Node* call, BasicBlock* continuation,
                                    BasicBlock* deoptimization) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32AddWithOverflow(Node* node,
                                                    FlagsContinuation* cont) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitInt32SubWithOverflow(Node* node,
                                                    FlagsContinuation* cont) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Test(Node* node, FlagsContinuation* cont) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitWord32Compare(Node* node,
                                             FlagsContinuation* cont) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitFloat64Compare(Node* node,
                                              FlagsContinuation* cont) {
  TRACE_UNIMPL();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
