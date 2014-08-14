// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/code-generator.h"

#include "src/mips/macro-assembler-mips.h"
#include "src/compiler/code-generator-impl.h"
#include "src/compiler/gap-resolver.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/node-properties-inl.h"
#include "src/scopes.h"

namespace v8 {
namespace internal {
namespace compiler {

#define __ masm()->


#define TRACE_UNIMPL() PrintF("code_generator_mips: unimplemnted %s at line %d\n", \
    __FUNCTION__, __LINE__);



// Adds Arm-specific methods to convert InstructionOperands.
class MipsOperandConverter : public InstructionOperandConverter {
 public:
  MipsOperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}

  Operand InputImmediate(int index) {
    // plind - fix this.
    // Constant constant = ToConstant(instr_->InputAt(index));
    // switch (constant.type()) {
    //   case Constant::kInt32:
    //     return Operand(constant.ToInt32());
    //   case Constant::kFloat64:
    //     return Operand(
    //         isolate()->factory()->NewNumber(constant.ToFloat64(), TENURED));
    //   case Constant::kInt64:
    //   case Constant::kExternalReference:
    //   case Constant::kHeapObject:
    //     break;
    // }
    // UNREACHABLE();
    TRACE_UNIMPL();
    return Operand(zero_reg);
  }

  Operand InputOperand2(int first_index) {
    return Operand(zero_reg);
  }

  MemOperand InputOffset(int* first_index) {
    TRACE_UNIMPL();
    return MemOperand(v0);  // plind, hack.
  }

  MemOperand InputOffset() {
    int index = 0;
    TRACE_UNIMPL();
    return InputOffset(&index);  // plind - insanity here, how can this work?
  }

  MemOperand ToMemOperand(InstructionOperand* op) const {
    DCHECK(op != NULL);
    DCHECK(!op->IsRegister());
    DCHECK(!op->IsDoubleRegister());
    DCHECK(op->IsStackSlot() || op->IsDoubleStackSlot());
    // The linkage computes where all spill slots are located.
    FrameOffset offset = linkage()->GetFrameOffset(op->index(), frame(), 0);
    return MemOperand(offset.from_stack_pointer() ? sp : fp, offset.offset());
  }
};


// Assembles an instruction after register allocation, producing machine code.
void CodeGenerator::AssembleArchInstruction(Instruction* instr) {
  TRACE_UNIMPL();
}


// Assembles branches after an instruction.
void CodeGenerator::AssembleArchBranch(Instruction* instr,
                                       FlagsCondition condition) {
  TRACE_UNIMPL();
}


// Assembles boolean materializations after an instruction.
void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  TRACE_UNIMPL();
}


void CodeGenerator::AssemblePrologue() {
  TRACE_UNIMPL();
}


void CodeGenerator::AssembleReturn() {
  TRACE_UNIMPL();
}


void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  TRACE_UNIMPL();
}


void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  TRACE_UNIMPL();
}


void CodeGenerator::AddNopForSmiCodeInlining() {
  // On 32-bit ARM we do not insert nops for inlined Smi code. plind, what about MIPS?
  UNREACHABLE();
}

#undef __
}
}
}  // namespace v8::internal::compiler
