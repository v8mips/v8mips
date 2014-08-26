// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/bits.h"
#include "src/compiler/instruction-selector-impl.h"
#include "src/compiler/node-matchers.h"

namespace v8 {
namespace internal {
namespace compiler {

#define TRACE_UNIMPL() PrintF("UNIMPLEMENTED instr_sel: %s at line %d\n", \
    __FUNCTION__, __LINE__)

#define TRACE() PrintF("instr_sel: %s at line %d\n", \
    __FUNCTION__, __LINE__)


// Adds Mips-specific methods for generating InstructionOperands.
class MipsOperandGenerator V8_FINAL : public OperandGenerator {
 public:
  explicit MipsOperandGenerator(InstructionSelector* selector)
      : OperandGenerator(selector) {}

  InstructionOperand* UseOperand(Node* node) {
    if (CanBeImmediate(node)) {
      return UseImmediate(node);
    }
    return UseRegister(node);
  }

  // TODO(plind): this ugliness is due to ldc1/sdc1 being implemented as two
  //    word ops, 2nd with offset +4 from original. Both require 16-bit offset.
  //    Maybe this can be used in just load/store rather than everywhere.
  //    TEST=test-run-machops/RunLoadStoreFloat64Offset
  static bool is_int16_special(int value) {
    return is_int16(value + 4);
  }


  bool CanBeImmediate(Node* node) {
    int32_t value;
    switch (node->opcode()) {
      // TODO(plind): SMI number constants as immediates ??? .......................................
      case IrOpcode::kInt32Constant:
        value = ValueOf<int32_t>(node->op());
        if (is_int16_special(value)) {
          return true;
        }
      default:
        return false;
    }
  }

 private:
  bool ImmediateFitsAddrMode1Instruction(int32_t imm) const {
    TRACE_UNIMPL();
    return false;
  }
};


// TODO(plind): Using for Shifts, probably WRONG, need to qualify immediate shift value. See x64 shift.
static void VisitRRO(InstructionSelector* selector, ArchOpcode opcode, Node* node) {
  MipsOperandGenerator g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)),
                 g.UseOperand(node->InputAt(1)));
}


static void VisitBinop(InstructionSelector* selector, Node* node,
                       InstructionCode opcode, FlagsContinuation* cont) {
  MipsOperandGenerator g(selector);
  Int32BinopMatcher m(node);
  InstructionOperand* inputs[4];
  size_t input_count = 0;
  InstructionOperand* outputs[2];
  size_t output_count = 0;

  inputs[input_count++] = g.UseRegister(m.left().node());
  inputs[input_count++] = g.UseOperand(m.right().node());

  if (cont->IsBranch()) {
    inputs[input_count++] = g.Label(cont->true_block());
    inputs[input_count++] = g.Label(cont->false_block());
  }

  outputs[output_count++] = g.DefineAsRegister(node);
  if (cont->IsSet()) {
    outputs[output_count++] = g.DefineAsRegister(cont->result());
  }

  DCHECK_NE(0, input_count);
  DCHECK_NE(0, output_count);
  DCHECK_GE(ARRAY_SIZE(inputs), input_count);
  DCHECK_GE(ARRAY_SIZE(outputs), output_count);

  Instruction* instr = selector->Emit(cont->Encode(opcode), output_count,
                                      outputs, input_count, inputs);
  if (cont->IsBranch()) instr->MarkAsControl();
}


static void VisitBinop(InstructionSelector* selector, Node* node,
                       InstructionCode opcode) {
  FlagsContinuation cont;
  VisitBinop(selector, node, opcode, &cont);
}


void InstructionSelector::VisitLoad(Node* node) {
  TRACE();
  MachineType rep = RepresentationOf(OpParameter<MachineType>(node));
  MipsOperandGenerator g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);

  InstructionOperand* result = rep == kRepFloat64
                                   ? g.DefineAsDoubleRegister(node)
                                   : g.DefineAsRegister(node);

  ArchOpcode opcode;
  // TODO(titzer): signed/unsigned small loads
  switch (rep) {
    case kRepFloat64:
      opcode = kMipsLdc1;
      break;
    case kRepBit:  // Fall through.
    case kRepWord8:
      opcode = kMipsLbu;
      break;
    case kRepWord16:
      opcode = kMipsLhu;
      break;
    case kRepTagged:  // Fall through.
    case kRepWord32:
      opcode = kMipsLw;
      break;
    default:
      UNREACHABLE();
      return;
  }

  // TODO(plind): I don't undertand interchangeable base/offset here.
  if (g.CanBeImmediate(index)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI), result,
         g.UseRegister(base), g.UseImmediate(index));
  } else if (g.CanBeImmediate(base)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI), result,
         g.UseRegister(index), g.UseImmediate(base));
  } else {
    // TODO(plind): Improve: use of temp reg to hold base + large-offset.
    InstructionOperand* addr_reg = g.TempRegister();
    Emit(kMipsMov | AddressingModeField::encode(kMode_MRI), addr_reg,
         g.UseImmediate(index));
    Emit(kMipsAdd | AddressingModeField::encode(kMode_None), addr_reg,
         addr_reg, g.UseRegister(base));
    // Emit desired load opcode, using temp addr_reg.
    Emit(opcode | AddressingModeField::encode(kMode_MRI), result,
         addr_reg, g.TempImmediate(0));
  }

}

void InstructionSelector::VisitStore(Node* node) {
  TRACE();
  MipsOperandGenerator g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  StoreRepresentation store_rep = OpParameter<StoreRepresentation>(node);
  MachineType rep = RepresentationOf(store_rep.machine_type);
  if (store_rep.write_barrier_kind == kFullWriteBarrier) {
    DCHECK(rep == kRepTagged);
    // TODO(dcarney): refactor RecordWrite function to take temp registers
    //                and pass them here instead of using fixed regs
    // TODO(dcarney): handle immediate indices.
    InstructionOperand* temps[] = {g.TempRegister(t1), g.TempRegister(t2)};
    Emit(kMipsStoreWriteBarrier, NULL, g.UseFixed(base, t0),
         g.UseFixed(index, t1), g.UseFixed(value, t2), ARRAY_SIZE(temps),
         temps);
    return;
  }
  DCHECK_EQ(kNoWriteBarrier, store_rep.write_barrier_kind);
  InstructionOperand* val =
      rep == kRepFloat64 ? g.UseDoubleRegister(value) : g.UseRegister(value);

  ArchOpcode opcode;
  switch (rep) {
    case kRepFloat64:
      opcode = kMipsSdc1;
      break;
    case kRepBit:  // Fall through.
    case kRepWord8:
      opcode = kMipsSb;
      break;
    case kRepWord16:
      opcode = kMipsSh;
      break;
    case kRepTagged:  // Fall through.
    case kRepWord32:
      opcode = kMipsSw;
      break;
    default:
      UNREACHABLE();
      return;
  }

  // TODO(plind): see VisitLoad comment re interchangeable base/offset.
  if (g.CanBeImmediate(index)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI), NULL,
         g.UseRegister(base), g.UseImmediate(index), val);
  } else if (g.CanBeImmediate(base)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI), NULL,
         g.UseRegister(index), g.UseImmediate(base), val);
  } else {
    // TODO(plind): Improve: use of temp reg to hold base + large-offset.
    InstructionOperand* addr_reg = g.TempRegister();
    Emit(kMipsMov | AddressingModeField::encode(kMode_MRI), addr_reg,
         g.UseImmediate(index));
    Emit(kMipsAdd | AddressingModeField::encode(kMode_None), addr_reg,
         addr_reg, g.UseRegister(base));
    // Emit desired store opcode, using temp addr_reg.
    Emit(opcode | AddressingModeField::encode(kMode_MRI), NULL,
         addr_reg, g.TempImmediate(0), val);
  }
}


void InstructionSelector::VisitWord32And(Node* node) {
  TRACE();
  VisitBinop(this, node, kMipsAnd);
}


void InstructionSelector::VisitWord32Or(Node* node) {
  TRACE();
  VisitBinop(this, node, kMipsOr);
}


void InstructionSelector::VisitWord32Xor(Node* node) {
  TRACE();
  VisitBinop(this, node, kMipsXor);
}


void InstructionSelector::VisitWord32Shl(Node* node) {
  TRACE();
  VisitRRO(this, kMipsShl, node);
}


void InstructionSelector::VisitWord32Shr(Node* node) {
  TRACE();
  VisitRRO(this, kMipsShr, node);
}


void InstructionSelector::VisitWord32Sar(Node* node) {
  TRACE();
  VisitRRO(this, kMipsSar, node);
}


void InstructionSelector::VisitWord32Ror(Node* node) {
  TRACE();
  VisitRRO(this, kMipsRor, node);
}


void InstructionSelector::VisitInt32Add(Node* node) {
  TRACE();
  MipsOperandGenerator g(this);

  // TODO(plind): Consider multiply & add optimization from arm port.
  VisitBinop(this, node, kMipsAdd);
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
