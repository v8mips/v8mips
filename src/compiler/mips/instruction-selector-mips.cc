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


static void VisitRRR(InstructionSelector* selector, ArchOpcode opcode,
                     Node* node) {
  MipsOperandGenerator g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)),
                 g.UseRegister(node->InputAt(1)));
}


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
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

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
  MachineType rep = RepresentationOf(OpParameter<MachineType>(node));
  MachineType typ = TypeOf(OpParameter<MachineType>(node));
  MipsOperandGenerator g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);

  ArchOpcode opcode;
  switch (rep) {
    case kRepFloat32:
      opcode = kMipsLwc1;
      break;
    case kRepFloat64:
      opcode = kMipsLdc1;
      break;
    case kRepBit:  // Fall through.
    case kRepWord8:
      opcode = typ == kTypeUint32 ? kMipsLbu : kMipsLb;
      break;
    case kRepWord16:
      opcode = typ == kTypeUint32 ? kMipsLhu : kMipsLh;
      break;
    case kRepTagged:  // Fall through.
    case kRepWord32:
      opcode = kMipsLw;
      break;
    default:
      UNREACHABLE();
      return;
  }

  if (g.CanBeImmediate(index)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI),
         g.DefineAsRegister(node), g.UseRegister(base), g.UseImmediate(index));
  } else {
    // TODO(plind): This could be done via assembler, saving a reg alloc.
    InstructionOperand* addr_reg = g.TempRegister();
    Emit(kMipsMov | AddressingModeField::encode(kMode_MRI), addr_reg,
         g.UseImmediate(index));
    Emit(kMipsAdd | AddressingModeField::encode(kMode_None), addr_reg,
         addr_reg, g.UseRegister(base));
    // Emit desired load opcode, using temp addr_reg.
    Emit(opcode | AddressingModeField::encode(kMode_MRI),
         g.DefineAsRegister(node), addr_reg, g.TempImmediate(0));
  }

}

void InstructionSelector::VisitStore(Node* node) {
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
         g.UseFixed(index, t1), g.UseFixed(value, t2), arraysize(temps),
         temps);
    return;
  }
  DCHECK_EQ(kNoWriteBarrier, store_rep.write_barrier_kind);

  ArchOpcode opcode;
  switch (rep) {
    case kRepFloat32:
      opcode = kMipsSwc1;
      break;
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

  if (g.CanBeImmediate(index)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI), NULL,
         g.UseRegister(base), g.UseImmediate(index), g.UseRegister(value));
  } else {
    // TODO(plind): This could be done via assembler, saving a reg alloc.
    InstructionOperand* addr_reg = g.TempRegister();
    Emit(kMipsMov | AddressingModeField::encode(kMode_MRI), addr_reg,
         g.UseImmediate(index));
    Emit(kMipsAdd | AddressingModeField::encode(kMode_None), addr_reg,
         addr_reg, g.UseRegister(base));
    // Emit desired store opcode, using temp addr_reg.
    Emit(opcode | AddressingModeField::encode(kMode_MRI), NULL,
         addr_reg, g.TempImmediate(0), g.UseRegister(value));
  }
}


void InstructionSelector::VisitWord32And(Node* node) {
  VisitBinop(this, node, kMipsAnd);
}


void InstructionSelector::VisitWord32Or(Node* node) {
  VisitBinop(this, node, kMipsOr);
}


void InstructionSelector::VisitWord32Xor(Node* node) {
  VisitBinop(this, node, kMipsXor);
}


void InstructionSelector::VisitWord32Shl(Node* node) {
  VisitRRO(this, kMipsShl, node);
}


void InstructionSelector::VisitWord32Shr(Node* node) {
  VisitRRO(this, kMipsShr, node);
}


void InstructionSelector::VisitWord32Sar(Node* node) {
  VisitRRO(this, kMipsSar, node);
}


void InstructionSelector::VisitWord32Ror(Node* node) {
  VisitRRO(this, kMipsRor, node);
}


void InstructionSelector::VisitInt32Add(Node* node) {
  MipsOperandGenerator g(this);

  // TODO(plind): Consider multiply & add optimization from arm port.
  VisitBinop(this, node, kMipsAdd);
}


void InstructionSelector::VisitInt32Sub(Node* node) {
  VisitBinop(this, node, kMipsSub);
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
  MipsOperandGenerator g(this);
  Emit(kMipsInt32ToFloat64, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeUint32ToFloat64(Node* node) {
  MipsOperandGenerator g(this);
  Emit(kMipsUint32ToFloat64, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeFloat64ToInt32(Node* node) {
  MipsOperandGenerator g(this);
  Emit(kMipsFloat64ToInt32, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeFloat64ToUint32(Node* node) {
  MipsOperandGenerator g(this);
  Emit(kMipsFloat64ToUint32, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64Add(Node* node) {
  VisitRRR(this, kMipsFloat64Add, node);
}


void InstructionSelector::VisitFloat64Sub(Node* node) {
  VisitRRR(this, kMipsFloat64Sub, node);
}


void InstructionSelector::VisitFloat64Mul(Node* node) {
  VisitRRR(this, kMipsFloat64Mul, node);
}


void InstructionSelector::VisitFloat64Div(Node* node) {
  VisitRRR(this, kMipsFloat64Div, node);
}


void InstructionSelector::VisitFloat64Mod(Node* node) {
  TRACE_UNIMPL();
}


void InstructionSelector::VisitCall(Node* call, BasicBlock* continuation,
                                    BasicBlock* deoptimization) {
  MipsOperandGenerator g(this);
  CallDescriptor* descriptor = OpParameter<CallDescriptor*>(call);

  FrameStateDescriptor* frame_state_descriptor = NULL;
  if (descriptor->NeedsFrameState()) {
    frame_state_descriptor =
        GetFrameStateDescriptor(call->InputAt(descriptor->InputCount()));
  }

  CallBuffer buffer(zone(), descriptor, frame_state_descriptor);

  // Compute InstructionOperands for inputs and outputs.
  InitializeCallBuffer(call, &buffer, true, false, continuation,
                       deoptimization);

  // TODO(dcarney): might be possible to use claim/poke instead
  // Push any stack arguments.
  for (NodeVectorRIter input = buffer.pushed_nodes.rbegin();
       input != buffer.pushed_nodes.rend(); input++) {
    // TODO(plind): inefficient for MIPS, use MultiPush here.
    //    - Also need to align the stack. See arm64.
    //    - Maybe combine with arg slot stuff in DirectCEntry stub.
    Emit(kMipsPush, NULL, g.UseRegister(*input));
  }

  // Select the appropriate opcode based on the call type.
  InstructionCode opcode;
  switch (descriptor->kind()) {
    case CallDescriptor::kCallCodeObject: {
      opcode = kArchCallCodeObject;
      break;
    }
    case CallDescriptor::kCallAddress:
      opcode = kArchCallAddress;
      break;
    case CallDescriptor::kCallJSFunction:
      opcode = kArchCallJSFunction;
      break;
    default:
      UNREACHABLE();
      return;
  }
  opcode |= MiscField::encode(descriptor->deoptimization_support());

  // Emit the call instruction.
  Instruction* call_instr =
      Emit(opcode, buffer.outputs.size(), &buffer.outputs.front(),
           buffer.instruction_args.size(), &buffer.instruction_args.front());

  call_instr->MarkAsCall();
  if (deoptimization != NULL) {
    DCHECK(continuation != NULL);
    call_instr->MarkAsControl();
  }

  // Caller clean up of stack for C-style calls.
  if (descriptor->kind() == CallDescriptor::kCallAddress &&
      !buffer.pushed_nodes.empty()) {
    DCHECK(deoptimization == NULL && continuation == NULL);
    Emit(kArchDrop | MiscField::encode(buffer.pushed_nodes.size()), NULL);
  }
}


// TODO(plind): Refactor VisitBinop() to include this usage, and/or simplify
//    VisitBinop() to remove branching usage on other binops, which are not
//    supported for MIPS.
static void VisitBinopOverflow(InstructionSelector* selector, Node* node,
                               InstructionCode opcode,
                               FlagsContinuation* cont) {
  MipsOperandGenerator g(selector);
  Int32BinopMatcher m(node);
  InstructionOperand* inputs[5];
  size_t input_count = 0;
  InstructionOperand* outputs[2];
  size_t output_count = 0;

  inputs[input_count++] = g.UseRegister(m.left().node());
  inputs[input_count++] = g.UseRegister(m.right().node());
  // inputs[input_count++] = g.TempRegister();

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
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

  Instruction* instr = selector->Emit(cont->Encode(opcode), output_count,
                                      outputs, input_count, inputs);
  if (cont->IsBranch()) instr->MarkAsControl();
}


void InstructionSelector::VisitInt32AddWithOverflow(Node* node,
                                                    FlagsContinuation* cont) {

  VisitBinopOverflow(this, node, kMipsAddOvf, cont);
}


void InstructionSelector::VisitInt32SubWithOverflow(Node* node,
                                                    FlagsContinuation* cont) {
  VisitBinopOverflow(this, node, kMipsSubOvf, cont);
}


// Shared routine for multiple compare operations.
static void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                         InstructionOperand* left, InstructionOperand* right,
                         FlagsContinuation* cont) {
  MipsOperandGenerator g(selector);
  opcode = cont->Encode(opcode);
  if (cont->IsBranch()) {
    selector->Emit(opcode, NULL, left, right, g.Label(cont->true_block()),
                   g.Label(cont->false_block()))->MarkAsControl();
  } else {
    DCHECK(cont->IsSet());
    // TODO(plind): this clause WONT work for mips right now.
    selector->Emit(opcode, g.DefineAsRegister(cont->result()), left, right);
  }
}


// Shared routine for multiple word compare operations.
static void VisitWordCompare(InstructionSelector* selector, Node* node,
                             InstructionCode opcode, FlagsContinuation* cont,
                             bool commutative) {
  MipsOperandGenerator g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);

  // Match immediates on left or right side of comparison.
  if (g.CanBeImmediate(right)) {
    VisitCompare(selector, opcode, g.UseRegister(left), g.UseImmediate(right),
                 cont);
  } else if (g.CanBeImmediate(left)) {
    if (!commutative) cont->Commute();
    VisitCompare(selector, opcode, g.UseRegister(right), g.UseImmediate(left),
                 cont);
  } else {
    VisitCompare(selector, opcode, g.UseRegister(left), g.UseRegister(right),
                 cont);
  }
}


void InstructionSelector::VisitWord32Test(Node* node, FlagsContinuation* cont) {
  switch (node->opcode()) {
    case IrOpcode::kWord32And:
      // TODO(plind): understand the significance of this 'IR and' special case.)
      return VisitWordCompare(this, node, kMipsTst, cont, true);
    default:
      break;
  }

  MipsOperandGenerator g(this);
  // kMipsTst is a pseudo-instruction to do logical 'and' and leave the result
  // in tmp register at.
  VisitCompare(this, kMipsTst, g.UseRegister(node), g.UseRegister(node),
               cont);
}


void InstructionSelector::VisitWord32Compare(Node* node,
                                             FlagsContinuation* cont) {
  VisitWordCompare(this, node, kMipsCmp, cont, false);
}


void InstructionSelector::VisitFloat64Compare(Node* node,
                                              FlagsContinuation* cont) {
  MipsOperandGenerator g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  VisitCompare(this, kMipsFloat64Cmp, g.UseRegister(left),
               g.UseRegister(right), cont);
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
