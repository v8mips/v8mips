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


// TODO(plind): Should NOT use these lithium names, change within assembler.
#define kScratchReg kLithiumScratchReg
#define kCompareReg kLithiumScratchReg2
#define kScratchDoubleReg kLithiumScratchDouble

// Use carefully, in areas that won't conflict with Branch & Booleans.
#define kScratchReg2 kCompareReg



// TODO(plind): remove these debug lines.

#if 1
#define TRACE() PrintF("code_gen: %s at line %d\n", \
    __FUNCTION__, __LINE__)

#define TRACE_MSG(msg) PrintF("code_gen: \'%s\' in function %s at line %d\n", \
    msg, __FUNCTION__, __LINE__)

#else
#define TRACE()
#define TRACE_MSG(msg)
#endif

#define TRACE_UNIMPL() PrintF("UNIMPLEMENTED code_generator_mips: %s at line %d\n", \
    __FUNCTION__, __LINE__)



// Adds Mips-specific methods to convert InstructionOperands.
class MipsOperandConverter : public InstructionOperandConverter {
 public:
  MipsOperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}

  Operand InputImmediate(int index) {
    Constant constant = ToConstant(instr_->InputAt(index));
    switch (constant.type()) {
      case Constant::kInt32:
        return Operand(constant.ToInt32());
      case Constant::kFloat64:
        return Operand(
            isolate()->factory()->NewNumber(constant.ToFloat64(), TENURED));
      case Constant::kInt64:
      case Constant::kExternalReference:
      case Constant::kHeapObject:
        // TODO(plind): Maybe we should handle ExtRef & HeapObj here?
        //    maybe not done on arm due to const pool ??
        break;
    }
    UNREACHABLE();
    return Operand(zero_reg);
  }

  Operand InputOperand(int index) {
    InstructionOperand* op = instr_->InputAt(index);
    if (op->IsRegister()) {
      return Operand(ToRegister(op));
    }
    return InputImmediate(index);  // TODO(plind): make this InputImmediate name & param more symmetical
  }

  MemOperand MemoryOperand(int* first_index) {
    const int index = *first_index;
    switch (AddressingModeField::decode(instr_->opcode())) {
      case kMode_None:
        break;
      case kMode_MRI:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputInt32(index + 1));
      case kMode_MRR:
        // TODO(plind): r6 address mode, to be implemented ...
        UNREACHABLE();
    }
    UNREACHABLE();
    return MemOperand(no_reg);
  }

  MemOperand MemoryOperand() {
    int index = 0;
    return MemoryOperand(&index);
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


static inline bool HasRegisterInput(Instruction* instr, int index) {
  return instr->InputAt(index)->IsRegister();
}


// TODO(plind): There are only 3 shift ops, does that justify this slightly
//    messy macro? Consider expanding this in place for sll, srl, sra ops.
#define ASSEMBLE_SHIFT(asm_instr)                                              \
  do {                                                                         \
    if (instr->InputAt(1)->IsRegister()) {                                     \
      __ asm_instr##v(i.OutputRegister(), i.InputRegister(0),                  \
                   i.InputRegister(1));                                        \
    } else {                                                                   \
      int32_t imm = i.InputOperand(1).immediate();                             \
      __ asm_instr(i.OutputRegister(), i.InputRegister(0), imm);               \
    }                                                                          \
  } while (0);


// Assembles an instruction after register allocation, producing machine code.
void CodeGenerator::AssembleArchInstruction(Instruction* instr) {
  MipsOperandConverter i(this, instr);
  InstructionCode opcode = instr->opcode();

  switch (ArchOpcodeField::decode(opcode)) {
    case kArchCallAddress: {
      DirectCEntryStub stub(isolate());
      stub.GenerateCall(masm(), i.InputRegister(0));
      __ Drop(kCArgSlotCount);  // TODO(plind): speculative - needs to be tested!! ............
      break;
    }
    case kArchCallCodeObject: {
      if (instr->InputAt(0)->IsImmediate()) {
        __ Call(Handle<Code>::cast(i.InputHeapObject(0)),
                RelocInfo::CODE_TARGET);
      } else {
        __ addiu(at, i.InputRegister(0), Code::kHeaderSize - kHeapObjectTag);
        __ Call(at);
      }
      AddSafepointAndDeopt(instr);
      break;
    }
    case kArchCallJSFunction: {
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ lw(kScratchReg, FieldMemOperand(func, JSFunction::kContextOffset));
        __ Assert(eq, kWrongFunctionContext, cp, Operand(kScratchReg));
      }

      __ lw(at, FieldMemOperand(func, JSFunction::kCodeEntryOffset));
      __ Call(at);
      AddSafepointAndDeopt(instr);
      break;
    }
    case kArchDrop: {
      int words = MiscField::decode(instr->opcode());
      __ Drop(words);
      DCHECK_LT(0, words);
      break;
    }
    case kArchJmp:
      __ Branch(code_->GetLabel(i.InputBlock(0)));
      break;
    case kArchNop:
      // don't emit code for nops.
      break;
    case kArchRet:
      AssembleReturn();
      break;
    case kArchTruncateDoubleToI:
      __ TruncateDoubleToI(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kMipsAdd:
      __ Addu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsAddOvf:
      __ AdduAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0),
                                 i.InputRegister(1), kCompareReg, kScratchReg);
      break;
    case kMipsSub:
      __ Subu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsSubOvf:
      __ SubuAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0),
                                 i.InputRegister(1), kCompareReg, kScratchReg);
      break;
    case kMipsMul:
      __ Mul(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsDiv:
      __ Div(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsDivU:
      __ Divu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsMod:
      __ Mod(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsModU:
      __ Modu(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsAnd:
      __ And(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsOr:
      __ Or(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsXor:
      __ Xor(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsShl:
      ASSEMBLE_SHIFT(sll);
      break;
    case kMipsShr:
      ASSEMBLE_SHIFT(srl);
      break;
    case kMipsSar:
      ASSEMBLE_SHIFT(sra);
      break;
    case kMipsRor:
      __ Ror(i.OutputRegister(), i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsTst:
      // Psuedo-instruction used for tst/branch.
      __ And(kCompareReg, i.InputRegister(0), i.InputOperand(1));
      break;
    case kMipsCmp:
      // Psuedo-instruction used for cmp/branch. No opcode emitted here.
      break;
    case kMipsMov:
      // TODO(plind): Should we combine mov/li like this, or use separate instr?
      //    - Also see x64 ASSEMBLE_BINOP & RegisterOrOperandType
      if (HasRegisterInput(instr, 0)) {
        __ mov(i.OutputRegister(), i.InputRegister(0));
      } else {
        __ li(i.OutputRegister(), i.InputOperand(0));
      }
      break;

    case kMipsFloat64Cmp:
      // Psuedo-instruction used for FP cmp/branch. No opcode emitted here.
      break;
    case kMipsFloat64Add:
    // TODO(plind): add special case: combine mult & add.
      __ add_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
              i.InputDoubleRegister(1));
      break;
    case kMipsFloat64Sub:
      __ sub_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
              i.InputDoubleRegister(1));
      break;
    case kMipsFloat64Mul:
      // TODO(plind): add special case: right op is -1.0, see arm port.
      __ mul_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
              i.InputDoubleRegister(1));
      break;
    case kMipsFloat64Div:
      __ div_d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
              i.InputDoubleRegister(1));
      break;
    case kMipsFloat64Mod: {
      // TODO(bmeurer): We should really get rid of this special instruction,
      // and generate a CallAddress instruction instead.
      FrameScope scope(masm(), StackFrame::MANUAL);
      __ PrepareCallCFunction(0, 2, kScratchReg);
      __ MovToFloatParameters(i.InputDoubleRegister(0),
                              i.InputDoubleRegister(1));
      __ CallCFunction(ExternalReference::mod_two_doubles_operation(isolate()),
                       0, 2);
      // Move the result in the double result register.
      __ MovFromFloatResult(i.OutputDoubleRegister());
      break;
    }
    case kMipsInt32ToFloat64: {
      FPURegister scratch = kScratchDoubleReg;
      __ mtc1(i.InputRegister(0), scratch);
      __ cvt_d_w(i.OutputDoubleRegister(), scratch);
      break;
    }
    case kMipsUint32ToFloat64: {
      FPURegister scratch = kScratchDoubleReg;
      __ Cvt_d_uw(i.OutputDoubleRegister(), i.InputRegister(0), scratch);
      break;
    }
    case kMipsFloat64ToInt32: {
      FPURegister scratch = kScratchDoubleReg;
      __ cvt_w_d(scratch, i.InputDoubleRegister(0));
      __ mfc1(i.OutputRegister(), scratch);
      break;
    }
    case kMipsFloat64ToUint32: {
      FPURegister scratch = kScratchDoubleReg;
      // TODO(plind): Fix wrong param order of Trunc_uw_d() macro-asm function.
      __ Trunc_uw_d(i.InputDoubleRegister(0), i.OutputRegister(), scratch);
      break;
    }
    // ... more basic instructions ...

    case kMipsLbu:
      __ lbu(i.OutputRegister(), i.MemoryOperand());
      break;
    case kMipsLb:
      __ lb(i.OutputRegister(), i.MemoryOperand());
      break;
    case kMipsSb:
      __ sb(i.InputRegister(2), i.MemoryOperand());
      break;
    case kMipsLhu:
      __ lhu(i.OutputRegister(), i.MemoryOperand());
      break;
    case kMipsLh:
      __ lh(i.OutputRegister(), i.MemoryOperand());
      break;
    case kMipsSh:
      __ sh(i.InputRegister(2), i.MemoryOperand());
      break;
    case kMipsLw:
      __ lw(i.OutputRegister(), i.MemoryOperand());
      break;
    case kMipsSw:
      __ sw(i.InputRegister(2), i.MemoryOperand());
      break;
    case kMipsLwc1: {
      FPURegister scratch = kScratchDoubleReg;
      __ lwc1(scratch, i.MemoryOperand());
      __ cvt_d_w(i.OutputDoubleRegister(), scratch);
      break;
    }
    case kMipsSwc1: {
      int index = 0;
      FPURegister scratch = kScratchDoubleReg;
      MemOperand operand = i.MemoryOperand(&index);
      __ cvt_w_d(scratch, i.InputDoubleRegister(index));
      __ sdc1(scratch, operand);
      break;
    }
    case kMipsLdc1:
      __ ldc1(i.OutputDoubleRegister(), i.MemoryOperand());
      break;
    case kMipsSdc1:
      __ sdc1(i.InputDoubleRegister(2), i.MemoryOperand());
      break;
     case kMipsPush:
      __ Push(i.InputRegister(0));
      break;
    case kMipsStoreWriteBarrier:
      Register object = i.InputRegister(0);
      Register index = i.InputRegister(1);
      Register value = i.InputRegister(2);
      __ addu(index, object, index);
      __ sw(value, MemOperand(index));
      SaveFPRegsMode mode =
          frame()->DidAllocateDoubleRegisters() ? kSaveFPRegs : kDontSaveFPRegs;
      RAStatus ra_status = kRAHasNotBeenSaved;
      __ RecordWrite(object, index, value, ra_status, mode);
      break;
  }
}


// Assembles branches after an instruction.
void CodeGenerator::AssembleArchBranch(Instruction* instr,
                                       FlagsCondition condition) {
  MipsOperandConverter i(this, instr);
  Label done;

  // Emit a branch. The true and false targets are always the last two inputs
  // to the instruction.
  BasicBlock* tblock = i.InputBlock(instr->InputCount() - 2);
  BasicBlock* fblock = i.InputBlock(instr->InputCount() - 1);
  bool fallthru = IsNextInAssemblyOrder(fblock);
  Label* tlabel = code()->GetLabel(tblock);
  Label* flabel = fallthru ? &done : code()->GetLabel(fblock);

  // MIPS does not have condition code flags, so compare and branch are
  // implemented differently than on the other arch's. The compare operations
  // emit mips psuedo-instructions, which are checked and handled here.
  // TODO(plind): Add CHECK() to ensure that test/cmp and this branch were
  //    not separated by other instructions.
  if (instr->arch_opcode() == kMipsTst) {
    // The kMipsTst psuedo-instruction emits And to 'kCompareReg' register.
    switch (condition) {
      case kNotEqual:
        __ Branch(tlabel, ne, kCompareReg, Operand(zero_reg));
        break;
      case kEqual:
        __ Branch(tlabel, eq, kCompareReg, Operand(zero_reg));
        break;
      default:
        // TODO(plind): Find debug printing support for text condition codes.
        PrintF("Unsupported kMipsTst condition: %d\n", condition);
        UNIMPLEMENTED();
        break;
    }
  } else if (instr->arch_opcode() == kMipsAddOvf ||
             instr->arch_opcode() == kMipsSubOvf) {
    // kMipsAddOvf, SubOvf emits negative result to 'kCompareReg' on overflow.
    switch (condition) {
      case kOverflow:
        __ Branch(tlabel, lt, kCompareReg, Operand(zero_reg));
        break;
      case kNotOverflow:
        __ Branch(tlabel, ge, kCompareReg, Operand(zero_reg));
        break;
      default:
        // TODO(plind): Find debug printing support for text condition codes.
        PrintF("Unsupported kMipsAdd/SubOvf condition: %d\n", condition);
        UNIMPLEMENTED();
        break;
    }
  } else if (instr->arch_opcode() == kMipsCmp) {
    switch (condition) {
      case kEqual:
        __ Branch(tlabel, eq, i.InputRegister(0), i.InputOperand(1));
        break;
      case kNotEqual:
        __ Branch(tlabel, ne, i.InputRegister(0), i.InputOperand(1));
        break;
      case kSignedLessThan:
        __ Branch(tlabel, lt, i.InputRegister(0), i.InputOperand(1));
        break;
      case kSignedGreaterThanOrEqual:
        __ Branch(tlabel, ge, i.InputRegister(0), i.InputOperand(1));
        break;
      case kSignedLessThanOrEqual:
        __ Branch(tlabel, le, i.InputRegister(0), i.InputOperand(1));
        break;
      case kSignedGreaterThan:
        __ Branch(tlabel, gt, i.InputRegister(0), i.InputOperand(1));
        break;
      case kUnsignedLessThan:
        __ Branch(tlabel, lo, i.InputRegister(0), i.InputOperand(1));
        break;
      case kUnsignedGreaterThanOrEqual:
        __ Branch(tlabel, hs, i.InputRegister(0), i.InputOperand(1));
        break;
      case kUnsignedLessThanOrEqual:
        __ Branch(tlabel, ls, i.InputRegister(0), i.InputOperand(1));
        break;
      case kUnsignedGreaterThan:
        __ Branch(tlabel, hi, i.InputRegister(0), i.InputOperand(1));
        break;
      case kOverflow:
      case kNotOverflow:
        TRACE_MSG("Under/Overflow not implemented on integer compare.\n");
        UNIMPLEMENTED();
        break;
      case kUnorderedEqual:
      case kUnorderedNotEqual:
      case kUnorderedLessThan:
      case kUnorderedGreaterThanOrEqual:
      case kUnorderedLessThanOrEqual:
      case kUnorderedGreaterThan:
        TRACE_MSG("Unordered tests not implemented on integer compare.\n");
        UNIMPLEMENTED();
        break;
    }
    if (!fallthru) __ Branch(flabel);  // no fallthru to flabel.
    __ bind(&done);

  } else if (instr->arch_opcode() == kMipsFloat64Cmp) {
    Label *nan = NULL;
    switch (condition) {
      case kUnorderedEqual:
        nan = flabel;
      // Fall through.
      case kEqual:
        __ BranchF(tlabel, nan, eq,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kUnorderedNotEqual:
        nan = tlabel;
      // Fall through.
      case kNotEqual:
        __ BranchF(tlabel, nan, ne,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kSignedLessThan:
        __ BranchF(tlabel, nan, lt,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kSignedGreaterThanOrEqual:
        __ BranchF(tlabel, nan, ge,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kSignedLessThanOrEqual:
        __ BranchF(tlabel, nan, le,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kSignedGreaterThan:
        __ BranchF(tlabel, nan, gt,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kUnorderedLessThan:
        nan = flabel;
      // Fall through.
      case kUnsignedLessThan:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(tlabel, nan, lo,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kUnorderedGreaterThanOrEqual:
        nan = tlabel;
      // Fall through.
      case kUnsignedGreaterThanOrEqual:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(tlabel, nan, hs,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kUnorderedLessThanOrEqual:
        nan = flabel;
      // Fall through.
      case kUnsignedLessThanOrEqual:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(tlabel, nan, ls,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kUnorderedGreaterThan:
      // Fall through.
      case kUnsignedGreaterThan:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(tlabel, nan, hi,
                   i.InputDoubleRegister(0), i.InputDoubleRegister(1));
        break;
      case kOverflow:
      case kNotOverflow:
        TRACE_MSG("Under/Overflow not implemented on FP compare.\n");
        UNIMPLEMENTED();
        break;
    }
    if (!fallthru) __ Branch(flabel);  // no fallthru to flabel.
    __ bind(&done);

  } else {
    PrintF("AssembleArchBranch Unimplemented arch_opcode is : %d\n",
           instr->arch_opcode());
    TRACE_UNIMPL();
    UNIMPLEMENTED();
  }


}


// Assembles boolean materializations after an instruction.
void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  MipsOperandConverter i(this, instr);
  Label done;

  // Materialize a full 32-bit 1 or 0 value. The result register is always the
  // last output of the instruction.
  Label set_false;
  DCHECK_NE(0, instr->OutputCount());
  Register result = i.OutputRegister(instr->OutputCount() - 1);
  // Condition cc = kNoCondition;  // TODO(plind): Optimize this routine using cc, make the code read simpler.

  // MIPS does not have condition code flags, so compare and branch are
  // implemented differently than on the other arch's. The compare operations
  // emit mips psuedo-instructions, which are checked and handled here.

  // For materializations, we use delay slot to set the result true, and
  // in the false case, where we fall thru the branch, we reset the result
  // false.

  // TODO(plind): Add CHECK() to ensure that test/cmp and this branch were
  //    not separated by other instructions.
  if (instr->arch_opcode() == kMipsTst) {
    // The kMipsTst psuedo-instruction emits And to 'kCompareReg' register.
    switch (condition) {
      case kNotEqual:
        __ Branch(USE_DELAY_SLOT, &done, ne, kCompareReg, Operand(zero_reg));
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kEqual:
        __ Branch(USE_DELAY_SLOT, &done, eq, kCompareReg, Operand(zero_reg));
        __ li(result, Operand(1));  // In delay slot.
        break;
      default:
        // TODO(plind): Find debug printing support for text condition codes.
        PrintF("Unsupported kMipsTst condition: %d\n", condition);
        UNIMPLEMENTED();
        break;
    }
  } else if (instr->arch_opcode() == kMipsAddOvf ||
             instr->arch_opcode() == kMipsSubOvf) {
    // kMipsAddOvf, SubOvf emits negative result to 'kCompareReg' on overflow.
    switch (condition) {
      case kOverflow:
        __ Branch(USE_DELAY_SLOT, &done, lt, kCompareReg, Operand(zero_reg));
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kNotOverflow:
        __ Branch(USE_DELAY_SLOT, &done, ge, kCompareReg, Operand(zero_reg));
        __ li(result, Operand(1));  // In delay slot.
        break;
      default:
        // TODO(plind): Find debug printing support for text condition codes.
        PrintF("Unsupported kMipsAdd/SubOvf condition: %d\n", condition);
        UNIMPLEMENTED();
        break;
    }
  } else if (instr->arch_opcode() == kMipsCmp) {
    Register left = i.InputRegister(0);
    Operand right = i.InputOperand(1);
    switch (condition) {
      // TODO(plind): this can be totally cleaned up to a single branch to 'cc' ......
      case kEqual:
        __ Branch(USE_DELAY_SLOT, &done, eq, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kNotEqual:
        __ Branch(USE_DELAY_SLOT, &done, ne, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedLessThan:
        __ Branch(USE_DELAY_SLOT, &done, lt, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedGreaterThanOrEqual:
        __ Branch(USE_DELAY_SLOT, &done, ge, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedLessThanOrEqual:
        __ Branch(USE_DELAY_SLOT, &done, le, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedGreaterThan:
        __ Branch(USE_DELAY_SLOT, &done, gt, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnsignedLessThan:
        __ Branch(USE_DELAY_SLOT, &done, lo, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnsignedGreaterThanOrEqual:
        __ Branch(USE_DELAY_SLOT, &done, hs, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnsignedLessThanOrEqual:
        __ Branch(USE_DELAY_SLOT, &done, ls, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnsignedGreaterThan:
        __ Branch(USE_DELAY_SLOT, &done, hi, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kOverflow:
      case kNotOverflow:
        TRACE_MSG("Under/Overflow not implemented on integer compare.\n");
        UNIMPLEMENTED();
        break;
      case kUnorderedEqual:
      case kUnorderedNotEqual:
      case kUnorderedLessThan:
      case kUnorderedGreaterThanOrEqual:
      case kUnorderedLessThanOrEqual:
      case kUnorderedGreaterThan:
        TRACE_MSG("Unordered tests not implemented on integer compare.\n");
        UNIMPLEMENTED();
        break;
    }
  } else if (instr->arch_opcode() == kMipsFloat64Cmp) {
    FPURegister left = i.InputDoubleRegister(0);
    FPURegister right = i.InputDoubleRegister(1);
    switch (condition) {
      case kUnorderedEqual:
      // TODO(plind):  HANDLE the NaN junk - not handled now.
      // Fall through.
      case kEqual:
        __ BranchF(USE_DELAY_SLOT, &done, NULL, eq, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnorderedNotEqual:
      // TODO(plind):  HANDLE the NaN junk - not handled now.
      // Fall through.
      case kNotEqual:
        __ BranchF(USE_DELAY_SLOT, &done, NULL, ne, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedLessThan:
        __ BranchF(USE_DELAY_SLOT, &done, NULL, lt, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedGreaterThanOrEqual:
        __ BranchF(USE_DELAY_SLOT, &done, NULL, ge, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedLessThanOrEqual:
        __ BranchF(USE_DELAY_SLOT, &done, NULL, le, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kSignedGreaterThan:
        __ BranchF(USE_DELAY_SLOT, &done, NULL, gt, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnorderedLessThan:
      // TODO(plind):  HANDLE the NaN junk - not handled now.
      // Fall through.
      case kUnsignedLessThan:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(USE_DELAY_SLOT, &done, NULL, lo, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnorderedGreaterThanOrEqual:
      // TODO(plind):  HANDLE the NaN junk - not handled now.
      // Fall through.
      case kUnsignedGreaterThanOrEqual:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(USE_DELAY_SLOT, &done, NULL, hs, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnorderedLessThanOrEqual:
      // TODO(plind):  HANDLE the NaN junk - not handled now.
      // Fall through.
      case kUnsignedLessThanOrEqual:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(USE_DELAY_SLOT, &done, NULL, ls, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kUnorderedGreaterThan:
      // Fall through.
      case kUnsignedGreaterThan:
        // TODO(plind): This is BROKEN: we have no unsigned compare for doubles.
        __ BranchF(USE_DELAY_SLOT, &done, NULL, hi, left, right);
        __ li(result, Operand(1));  // In delay slot.
        break;
      case kOverflow:
      case kNotOverflow:
        TRACE_MSG("Under/Overflow not implemented on FP compare.\n");
        UNIMPLEMENTED();
        break;
    }
  } else {
    PrintF("AssembleArchBranch Unimplemented arch_opcode is : %d\n",
           instr->arch_opcode());
    TRACE_UNIMPL();
    UNIMPLEMENTED();
  }
  // Fallthru case is the false materialization.
  __ li(result, Operand(0));
  __ bind(&done);

}


void CodeGenerator::AssembleDeoptimizerCall(int deoptimization_id) {
  Address deopt_entry = Deoptimizer::GetDeoptimizationEntry(
      isolate(), deoptimization_id, Deoptimizer::LAZY);
  __ Call(deopt_entry, RelocInfo::RUNTIME_ENTRY);
}


void CodeGenerator::AssemblePrologue() {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();
  if (descriptor->kind() == CallDescriptor::kCallAddress) {
    __ Push(ra, fp);
    __ mov(fp, sp);
    const RegList saves = descriptor->CalleeSavedRegisters();
    if (saves != 0) {  // Save callee-saved registers.
      // TODO(plind): make callee save size const, possibly DCHECK it.
      int register_save_area_size = 0;
      for (int i = Register::kNumRegisters - 1; i >= 0; i--) {
        if (!((1 << i) & saves)) continue;
        register_save_area_size += kPointerSize;
      }
      frame()->SetRegisterSaveAreaSize(register_save_area_size);
      __ MultiPush(saves);
    }
  } else if (descriptor->IsJSFunctionCall()) {
    CompilationInfo* info = linkage()->info();
    __ Prologue(info->IsCodePreAgingActive());
    frame()->SetRegisterSaveAreaSize(
        StandardFrameConstants::kFixedFrameSizeFromFp);

    // Sloppy mode functions and builtins need to replace the receiver with the
    // global proxy when called as functions (without an explicit receiver
    // object).
    // TODO(mstarzinger/verwaest): Should this be moved back into the CallIC?
    if (info->strict_mode() == SLOPPY && !info->is_native()) {
      Label ok;
      // +2 for return address and saved frame pointer.
      int receiver_slot = info->scope()->num_parameters() + 2;
      __ lw(a2, MemOperand(fp, receiver_slot * kPointerSize));
      __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
      __ Branch(&ok, ne, a2, Operand(at));

      __ lw(a2, GlobalObjectOperand());
      __ lw(a2, FieldMemOperand(a2, GlobalObject::kGlobalProxyOffset));
      __ sw(a2, MemOperand(fp, receiver_slot * kPointerSize));
      __ bind(&ok);
    }

  } else {
    __ StubPrologue();
    frame()->SetRegisterSaveAreaSize(
        StandardFrameConstants::kFixedFrameSizeFromFp);
  }
  int stack_slots = frame()->GetSpillSlotCount();
  if (stack_slots > 0) {
    __ Subu(sp, sp, Operand(stack_slots * kPointerSize));
  }
}


void CodeGenerator::AssembleReturn() {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();
  if (descriptor->kind() == CallDescriptor::kCallAddress) {
    if (frame()->GetRegisterSaveAreaSize() > 0) {
      // Remove this frame's spill slots first.
      int stack_slots = frame()->GetSpillSlotCount();
      if (stack_slots > 0) {
        __ Addu(sp, sp, Operand(stack_slots * kPointerSize));
      }
      // Restore registers.
      const RegList saves = descriptor->CalleeSavedRegisters();
      if (saves != 0) {
        __ MultiPop(saves);
      }
    }
    __ mov(sp, fp);
    __ Pop(ra, fp);
    __ Ret();
  } else {
    __ mov(sp, fp);
    __ Pop(ra, fp);
    int pop_count = descriptor->IsJSFunctionCall()
                        ? static_cast<int>(descriptor->JSParameterCount())
                        : 0;
    __ DropAndRet(pop_count);
  }
}


void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  MipsOperandConverter g(this, NULL);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      __ mov(g.ToRegister(destination), src);
    } else {
      __ sw(src, g.ToMemOperand(destination));
    }
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsRegister()) {
      __ lw(g.ToRegister(destination), src);
    } else {
      Register temp = kScratchReg;
      __ lw(temp, src);
      __ sw(temp, g.ToMemOperand(destination));
    }
  } else if (source->IsConstant()) {
    if (destination->IsRegister() || destination->IsStackSlot()) {
      Register dst =
          destination->IsRegister() ? g.ToRegister(destination) : kScratchReg;
      Constant src = g.ToConstant(source);
      switch (src.type()) {
        case Constant::kInt32:
          __ li(dst, Operand(src.ToInt32()));
          break;
        case Constant::kInt64:
          UNREACHABLE();
          break;
        case Constant::kFloat64:
          __ li(dst,
                isolate()->factory()->NewNumber(src.ToFloat64(), TENURED));
          break;
        case Constant::kExternalReference:
          __ li(dst, Operand(src.ToExternalReference()));
          break;
        case Constant::kHeapObject:
          __ li(dst, src.ToHeapObject());
          break;
      }
      if (destination->IsStackSlot()) __ sw(dst, g.ToMemOperand(destination));
    } else if (destination->IsDoubleRegister()) {
      FPURegister result = g.ToDoubleRegister(destination);
      __ Move(result, g.ToDouble(source));
    } else {
      DCHECK(destination->IsDoubleStackSlot());
      FPURegister temp = kScratchDoubleReg;
      __ Move(temp, g.ToDouble(source));
      __ sdc1(temp, g.ToMemOperand(destination));
    }
  } else if (source->IsDoubleRegister()) {
    FPURegister src = g.ToDoubleRegister(source);
    if (destination->IsDoubleRegister()) {
      FPURegister dst = g.ToDoubleRegister(destination);
      __ Move(dst, src);
    } else {
      DCHECK(destination->IsDoubleStackSlot());
      __ sdc1(src, g.ToMemOperand(destination));
    }
  } else if (source->IsDoubleStackSlot()) {
    DCHECK(destination->IsDoubleRegister() || destination->IsDoubleStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsDoubleRegister()) {
      __ ldc1(g.ToDoubleRegister(destination), src);
    } else {
      FPURegister temp = kScratchDoubleReg;
      __ ldc1(temp, src);
      __ sdc1(temp, g.ToMemOperand(destination));
    }
  } else {
    UNREACHABLE();
  }
}


void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  MipsOperandConverter g(this, NULL);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    // Register-register.
    Register temp = kScratchReg;
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      Register dst = g.ToRegister(destination);
      __ Move(temp, src);
      __ Move(src, dst);
      __ Move(dst, temp);
    } else {
      DCHECK(destination->IsStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ mov(temp, src);
      __ lw(src, dst);
      __ sw(temp, dst);
    }
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsStackSlot());
    Register temp_0 = kScratchReg;
    Register temp_1 = kCompareReg;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    __ lw(temp_0, src);
    __ lw(temp_1, dst);
    __ sw(temp_0, dst);
    __ sw(temp_1, src);
  } else if (source->IsDoubleRegister()) {
    FPURegister temp = kScratchDoubleReg;
    FPURegister src = g.ToDoubleRegister(source);
    if (destination->IsDoubleRegister()) {
      FPURegister dst = g.ToDoubleRegister(destination);
      __ Move(temp, src);
      __ Move(src, dst);
      __ Move(src, temp);
    } else {
      DCHECK(destination->IsDoubleStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ Move(temp, src);
      __ ldc1(src, dst);
      __ sdc1(temp, dst);
    }
  } else if (source->IsDoubleStackSlot()) {
    DCHECK(destination->IsDoubleStackSlot());
    Register temp_0 = kScratchReg;
    FPURegister temp_1 = kScratchDoubleReg;
    MemOperand src0 = g.ToMemOperand(source);
    MemOperand src1(src0.rm(), src0.offset() + kPointerSize);
    MemOperand dst0 = g.ToMemOperand(destination);
    MemOperand dst1(dst0.rm(), dst0.offset() + kPointerSize);
    __ ldc1(temp_1, dst0);  // Save destination in temp_1.
    __ lw(temp_0, src0);   // Then use temp_0 to copy source to destination.
    __ sw(temp_0, dst0);
    __ lw(temp_0, src1);
    __ sw(temp_0, dst1);
    __ sdc1(temp_1, src0);
  } else {
    // No other combinations are possible.
    UNREACHABLE();
  }
}


void CodeGenerator::AddNopForSmiCodeInlining() {
  // Unused on 32-bit ARM. Still exists on 64-bit arm.
  // TODO(plind): Unclear when this is called now. Understand, fix if needed.
  __ nop();  // Maybe PROPERTY_ACCESS_INLINED?
}

#undef __

}  // namespace compiler
}  // namespace internal
}  // namespace v8

