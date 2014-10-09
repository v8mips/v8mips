// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/bits.h"
#include "src/compiler/instruction-selector-impl.h"
#include "src/compiler/node-matchers.h"

namespace v8 {
namespace internal {
namespace compiler {

#define TRACE_UNIMPL() \
  PrintF("UNIMPLEMENTED instr_sel: %s at line %d\n", __FUNCTION__, __LINE__)

#define TRACE() PrintF("instr_sel: %s at line %d\n", __FUNCTION__, __LINE__)


// Adds Mips-specific methods for generating InstructionOperands.
class Mips64OperandGenerator FINAL : public OperandGenerator {
 public:
  explicit Mips64OperandGenerator(InstructionSelector* selector)
      : OperandGenerator(selector) {}

  InstructionOperand* UseOperand(Node* node, InstructionCode opcode) {
    if (CanBeImmediate(node, opcode)) {
      return UseImmediate(node);
    }
    return UseRegister(node);
  }

  bool CanBeImmediate(Node* node, InstructionCode opcode) {
    int64_t value;
    if (node->opcode() == IrOpcode::kInt32Constant)
      value = OpParameter<int32_t>(node);
    else if (node->opcode() == IrOpcode::kInt64Constant)
      value = OpParameter<int64_t>(node);
    else
      return false;
    switch (ArchOpcodeField::decode(opcode)) {
      case kMips64Shl:
      case kMips64Sar:
      case kMips64Shr:
        return is_uint5(value);
      case kMips64Xor:
        return is_uint16(value);
      case kMips64Ldc1:
      case kMips64Sdc1:
        return is_int16(value + kIntSize);
      default:
        return is_int16(value);
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
  Mips64OperandGenerator g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)),
                 g.UseRegister(node->InputAt(1)));
}


static void VisitRRO(InstructionSelector* selector, ArchOpcode opcode,
                     Node* node) {
  Mips64OperandGenerator g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)),
                 g.UseOperand(node->InputAt(1), opcode));
}


static void VisitBinop(InstructionSelector* selector, Node* node,
                       InstructionCode opcode, FlagsContinuation* cont) {
  Mips64OperandGenerator g(selector);
  Int32BinopMatcher m(node);
  InstructionOperand* inputs[4];
  size_t input_count = 0;
  InstructionOperand* outputs[2];
  size_t output_count = 0;

  inputs[input_count++] = g.UseRegister(m.left().node());
  inputs[input_count++] = g.UseOperand(m.right().node(), opcode);

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
  MachineType rep = RepresentationOf(OpParameter<LoadRepresentation>(node));
  MachineType typ = TypeOf(OpParameter<LoadRepresentation>(node));
  Mips64OperandGenerator g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);

  ArchOpcode opcode;
  switch (rep) {
    case kRepFloat32:
      opcode = kMips64Lwc1;
      break;
    case kRepFloat64:
      opcode = kMips64Ldc1;
      break;
    case kRepBit:  // Fall through.
    case kRepWord8:
      opcode = typ == kTypeUint32 ? kMips64Lbu : kMips64Lb;
      break;
    case kRepWord16:
      opcode = typ == kTypeUint32 ? kMips64Lhu : kMips64Lh;
      break;
    case kRepWord32:
      opcode = kMips64Lw;
      break;
    case kRepTagged:  // Fall through.
    case kRepWord64:
      opcode = kMips64Ld;
      break;
    default:
      UNREACHABLE();
      return;
  }

  if (g.CanBeImmediate(index, opcode)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI),
         g.DefineAsRegister(node), g.UseRegister(base), g.UseImmediate(index));
  } else {
    InstructionOperand* addr_reg = g.TempRegister();
    Emit(kMips64Dadd | AddressingModeField::encode(kMode_None), addr_reg,
         g.UseRegister(index), g.UseRegister(base));
    // Emit desired load opcode, using temp addr_reg.
    Emit(opcode | AddressingModeField::encode(kMode_MRI),
         g.DefineAsRegister(node), addr_reg, g.TempImmediate(0));
  }
}


void InstructionSelector::VisitStore(Node* node) {
  Mips64OperandGenerator g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  StoreRepresentation store_rep = OpParameter<StoreRepresentation>(node);
  MachineType rep = RepresentationOf(store_rep.machine_type());
  if (store_rep.write_barrier_kind() == kFullWriteBarrier) {
    DCHECK(rep == kRepTagged);
    // TODO(dcarney): refactor RecordWrite function to take temp registers
    //                and pass them here instead of using fixed regs
    // TODO(dcarney): handle immediate indices.
    InstructionOperand* temps[] = {g.TempRegister(t1), g.TempRegister(t2)};
    Emit(kMips64StoreWriteBarrier, NULL, g.UseFixed(base, t0),
         g.UseFixed(index, t1), g.UseFixed(value, t2), arraysize(temps), temps);
    return;
  }
  DCHECK_EQ(kNoWriteBarrier, store_rep.write_barrier_kind());

  ArchOpcode opcode;
  switch (rep) {
    case kRepFloat32:
      opcode = kMips64Swc1;
      break;
    case kRepFloat64:
      opcode = kMips64Sdc1;
      break;
    case kRepBit:  // Fall through.
    case kRepWord8:
      opcode = kMips64Sb;
      break;
    case kRepWord16:
      opcode = kMips64Sh;
      break;
    case kRepWord32:
      opcode = kMips64Sw;
      break;
    case kRepTagged:  // Fall through.
    case kRepWord64:
      opcode = kMips64Sd;
      break;
    default:
      UNREACHABLE();
      return;
  }

  if (g.CanBeImmediate(index, opcode)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI), NULL,
         g.UseRegister(base), g.UseImmediate(index), g.UseRegister(value));
  } else {
    InstructionOperand* addr_reg = g.TempRegister();
    Emit(kMips64Dadd | AddressingModeField::encode(kMode_None), addr_reg,
         g.UseRegister(index), g.UseRegister(base));
    // Emit desired store opcode, using temp addr_reg.
    Emit(opcode | AddressingModeField::encode(kMode_MRI), NULL, addr_reg,
         g.TempImmediate(0), g.UseRegister(value));
  }
}


void InstructionSelector::VisitWord32And(Node* node) {
  VisitBinop(this, node, kMips64And32);
}


void InstructionSelector::VisitWord64And(Node* node) {
  VisitBinop(this, node, kMips64And);
}


void InstructionSelector::VisitWord32Or(Node* node) {
  VisitBinop(this, node, kMips64Or32);
}


void InstructionSelector::VisitWord64Or(Node* node) {
  VisitBinop(this, node, kMips64Or);
}


void InstructionSelector::VisitWord32Xor(Node* node) {
  VisitBinop(this, node, kMips64Xor32);
}


void InstructionSelector::VisitWord64Xor(Node* node) {
  VisitBinop(this, node, kMips64Xor);
}


void InstructionSelector::VisitWord32Shl(Node* node) {
  VisitRRO(this, kMips64Shl, node);
}


void InstructionSelector::VisitWord32Shr(Node* node) {
  VisitRRO(this, kMips64Shr, node);
}


void InstructionSelector::VisitWord32Sar(Node* node) {
  VisitRRO(this, kMips64Sar, node);
}


void InstructionSelector::VisitWord64Shl(Node* node) {
  VisitRRO(this, kMips64Dshl, node);
}


void InstructionSelector::VisitWord64Shr(Node* node) {
  VisitRRO(this, kMips64Dshr, node);
}


void InstructionSelector::VisitWord64Sar(Node* node) {
  VisitRRO(this, kMips64Dsar, node);
}

void InstructionSelector::VisitWord32Ror(Node* node) {
  VisitRRO(this, kMips64Ror, node);
}


void InstructionSelector::VisitWord64Ror(Node* node) {
  VisitRRO(this, kMips64Dror, node);
}


void InstructionSelector::VisitInt32Add(Node* node) {
  Mips64OperandGenerator g(this);

  // TODO(plind): Consider multiply & add optimization from arm port.
  VisitBinop(this, node, kMips64Add);
}


void InstructionSelector::VisitInt64Add(Node* node) {
  Mips64OperandGenerator g(this);

  // TODO(plind): Consider multiply & add optimization from arm port.
  VisitBinop(this, node, kMips64Dadd);
}


void InstructionSelector::VisitInt32Sub(Node* node) {
  VisitBinop(this, node, kMips64Sub);
}


void InstructionSelector::VisitInt64Sub(Node* node) {
  VisitBinop(this, node, kMips64Dsub);
}


void InstructionSelector::VisitInt32Mul(Node* node) {
  Mips64OperandGenerator g(this);
  Int32BinopMatcher m(node);
  if (m.right().HasValue() && m.right().Value() > 0) {
    int32_t value = m.right().Value();
    if (base::bits::IsPowerOfTwo32(value)) {
      Emit(kMips64Shl | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()),
           g.TempImmediate(WhichPowerOf2(value)));
      return;
    }
    if (base::bits::IsPowerOfTwo32(value - 1)) {
      InstructionOperand* temp = g.TempRegister();
      Emit(kMips64Shl | AddressingModeField::encode(kMode_None), temp,
           g.UseRegister(m.left().node()),
           g.TempImmediate(WhichPowerOf2(value - 1)));
      Emit(kMips64Add | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()), temp);
      return;
    }
    if (base::bits::IsPowerOfTwo32(value + 1)) {
      InstructionOperand* temp = g.TempRegister();
      Emit(kMips64Shl | AddressingModeField::encode(kMode_None), temp,
           g.UseRegister(m.left().node()),
           g.TempImmediate(WhichPowerOf2(value + 1)));
      Emit(kMips64Sub | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), temp, g.UseRegister(m.left().node()));
      return;
    }
  }
  Emit(kMips64Mul, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitInt64Mul(Node* node) {
  Mips64OperandGenerator g(this);
  Int64BinopMatcher m(node);
  // TODO(dusmil): Add optimization for shifts larger than 32.
  if (m.right().HasValue() && m.right().Value() > 0) {
    int64_t value = m.right().Value();
    if (base::bits::IsPowerOfTwo32(value)) {
      Emit(kMips64Dshl | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()),
           g.TempImmediate(WhichPowerOf2(value)));
      return;
    }
    if (base::bits::IsPowerOfTwo32(value - 1)) {
      InstructionOperand* temp = g.TempRegister();
      Emit(kMips64Dshl | AddressingModeField::encode(kMode_None), temp,
           g.UseRegister(m.left().node()),
           g.TempImmediate(WhichPowerOf2(value - 1)));
      Emit(kMips64Dadd | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()), temp);
      return;
    }
    if (base::bits::IsPowerOfTwo32(value + 1)) {
      InstructionOperand* temp = g.TempRegister();
      Emit(kMips64Dshl | AddressingModeField::encode(kMode_None), temp,
           g.UseRegister(m.left().node()),
           g.TempImmediate(WhichPowerOf2(value + 1)));
      Emit(kMips64Dsub | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), temp, g.UseRegister(m.left().node()));
      return;
    }
  }
  Emit(kMips64Dmul, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

void InstructionSelector::VisitInt32Div(Node* node) {
  Mips64OperandGenerator g(this);
  Int32BinopMatcher m(node);
  Emit(kMips64Div, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitUint32Div(Node* node) {
  Mips64OperandGenerator g(this);
  Int32BinopMatcher m(node);
  Emit(kMips64DivU, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitInt32Mod(Node* node) {
  Mips64OperandGenerator g(this);
  Int32BinopMatcher m(node);
  Emit(kMips64Mod, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitUint32Mod(Node* node) {
  Mips64OperandGenerator g(this);
  Int32BinopMatcher m(node);
  Emit(kMips64ModU, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitInt64Div(Node* node) {
  Mips64OperandGenerator g(this);
  Int64BinopMatcher m(node);
  Emit(kMips64Ddiv, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitUint64Div(Node* node) {
  Mips64OperandGenerator g(this);
  Int64BinopMatcher m(node);
  Emit(kMips64DdivU, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitInt64Mod(Node* node) {
  Mips64OperandGenerator g(this);
  Int64BinopMatcher m(node);
  Emit(kMips64Dmod, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}


void InstructionSelector::VisitUint64Mod(Node* node) {
  Mips64OperandGenerator g(this);
  Int64BinopMatcher m(node);
  Emit(kMips64DmodU, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

void InstructionSelector::VisitChangeFloat32ToFloat64(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64CvtDS, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeInt32ToFloat64(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64CvtDW, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeUint32ToFloat64(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64CvtDUw, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeFloat64ToInt32(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64TruncWD, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeFloat64ToUint32(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64TruncUwD, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitChangeInt32ToInt64(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64Shl, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
      g.TempImmediate(0));
}


void InstructionSelector::VisitChangeUint32ToUint64(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64Ext, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
      g.TempImmediate(0), g.TempImmediate(32));
}


void InstructionSelector::VisitTruncateInt64ToInt32(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64Ext, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
      g.TempImmediate(0), g.TempImmediate(32));
}

void InstructionSelector::VisitTruncateFloat64ToFloat32(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64CvtSD, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64Add(Node* node) {
  VisitRRR(this, kMips64AddD, node);
}


void InstructionSelector::VisitFloat64Sub(Node* node) {
  VisitRRR(this, kMips64SubD, node);
}


void InstructionSelector::VisitFloat64Mul(Node* node) {
  VisitRRR(this, kMips64MulD, node);
}


void InstructionSelector::VisitFloat64Div(Node* node) {
  VisitRRR(this, kMips64DivD, node);
}


void InstructionSelector::VisitFloat64Mod(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64ModD, g.DefineAsFixed(node, f0), g.UseFixed(node->InputAt(0), f12),
       g.UseFixed(node->InputAt(1), f14))->MarkAsCall();
}


void InstructionSelector::VisitFloat64Sqrt(Node* node) {
  Mips64OperandGenerator g(this);
  Emit(kMips64SqrtD, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitCall(Node* call, BasicBlock* continuation,
                                    BasicBlock* deoptimization) {
  Mips64OperandGenerator g(this);
  CallDescriptor* descriptor = OpParameter<CallDescriptor*>(call);

  FrameStateDescriptor* frame_state_descriptor = NULL;
  if (descriptor->NeedsFrameState()) {
    frame_state_descriptor =
        GetFrameStateDescriptor(call->InputAt(descriptor->InputCount()));
  }

  CallBuffer buffer(zone(), descriptor, frame_state_descriptor);

  // Compute InstructionOperands for inputs and outputs.
  InitializeCallBuffer(call, &buffer, true, false);

  // TODO(dcarney): might be possible to use claim/poke instead
  // Push any stack arguments.
  for (NodeVectorRIter input = buffer.pushed_nodes.rbegin();
       input != buffer.pushed_nodes.rend(); input++) {
    // TODO(plind): inefficient for MIPS, use MultiPush here.
    //    - Also need to align the stack. See arm64.
    //    - Maybe combine with arg slot stuff in DirectCEntry stub.
    Emit(kMips64Push, NULL, g.UseRegister(*input));
  }

  // Select the appropriate opcode based on the call type.
  InstructionCode opcode;
  switch (descriptor->kind()) {
    case CallDescriptor::kCallCodeObject: {
      opcode = kArchCallCodeObject;
      break;
    }
    case CallDescriptor::kCallJSFunction:
      opcode = kArchCallJSFunction;
      break;
    default:
      UNREACHABLE();
      return;
  }
  opcode |= MiscField::encode(descriptor->flags());

  // Emit the call instruction.
  Instruction* call_instr =
      Emit(opcode, buffer.outputs.size(), &buffer.outputs.front(),
           buffer.instruction_args.size(), &buffer.instruction_args.front());

  call_instr->MarkAsCall();
  if (deoptimization != NULL) {
    DCHECK(continuation != NULL);
    call_instr->MarkAsControl();
  }
}


void InstructionSelector::VisitInt32AddWithOverflow(Node* node,
                                                    FlagsContinuation* cont) {
  VisitBinop(this, node, kMips64AddOvf, cont);
}


void InstructionSelector::VisitInt32SubWithOverflow(Node* node,
                                                    FlagsContinuation* cont) {
  VisitBinop(this, node, kMips64SubOvf, cont);
}


// Shared routine for multiple compare operations.
static void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                         InstructionOperand* left, InstructionOperand* right,
                         FlagsContinuation* cont) {
  Mips64OperandGenerator g(selector);
  opcode = cont->Encode(opcode);
  if (cont->IsBranch()) {
    selector->Emit(opcode, NULL, left, right, g.Label(cont->true_block()),
                   g.Label(cont->false_block()))->MarkAsControl();
  } else {
    DCHECK(cont->IsSet());
    // TODO(plind): Revisit and test this path.
    selector->Emit(opcode, g.DefineAsRegister(cont->result()), left, right);
  }
}


// Shared routine for multiple word compare operations.
static void VisitWordCompare(InstructionSelector* selector, Node* node,
                             InstructionCode opcode, FlagsContinuation* cont,
                             bool commutative) {
  Mips64OperandGenerator g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);

  // Match immediates on left or right side of comparison.
  if (g.CanBeImmediate(right, opcode)) {
    VisitCompare(selector, opcode, g.UseRegister(left), g.UseImmediate(right),
                 cont);
  } else if (g.CanBeImmediate(left, opcode)) {
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
      // TODO(plind): understand the significance of 'IR and' special case.
      return VisitWordCompare(this, node, kMips64Tst, cont, true);
    default:
      break;
  }

  Mips64OperandGenerator g(this);
  // kMips64Tst is a pseudo-instruction to do logical 'and' and leave the result
  // in a dedicated tmp register.
  VisitCompare(this, kMips64Tst, g.UseRegister(node), g.UseRegister(node), cont);
}


void InstructionSelector::VisitWord64Test(Node* node, FlagsContinuation* cont) {
  UNIMPLEMENTED();
}


void InstructionSelector::VisitWord32Compare(Node* node,
                                             FlagsContinuation* cont) {
  VisitWordCompare(this, node, kMips64Cmp, cont, false);
}


void InstructionSelector::VisitWord64Compare(Node* node,
                                             FlagsContinuation* cont) {
  UNIMPLEMENTED();
}


void InstructionSelector::VisitFloat64Compare(Node* node,
                                              FlagsContinuation* cont) {
  Mips64OperandGenerator g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  VisitCompare(this, kMips64CmpD, g.UseRegister(left), g.UseRegister(right),
               cont);
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
