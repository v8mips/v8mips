// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.



#include "v8.h"

#include "bootstrapper.h"
#include "codegen-inl.h"
#include "debug.h"
#include "runtime.h"

namespace v8 {
namespace internal {

MacroAssembler::MacroAssembler(void* buffer, int size)
    : Assembler(buffer, size),
      unresolved_(0),
      generating_stub_(false),
      allow_stub_calls_(true),
      code_object_(Heap::undefined_value()) {
}



void MacroAssembler::Jump(Register target, Condition cond,
                          Register r1, const Operand& r2) {
  Jump(Operand(target), cond, r1, r2);
}


void MacroAssembler::Jump(intptr_t target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2) {
  Jump(Operand(target, rmode), cond, r1, r2);
}


void MacroAssembler::Jump(byte* target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2) {
  ASSERT(!RelocInfo::IsCodeTarget(rmode));
  Jump(reinterpret_cast<intptr_t>(target), rmode, cond, r1, r2);
}


void MacroAssembler::Jump(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2) {
  ASSERT(RelocInfo::IsCodeTarget(rmode));
  Jump(reinterpret_cast<intptr_t>(code.location()), rmode, cond);
}


void MacroAssembler::Call(Register target,
                          Condition cond, Register r1, const Operand& r2) {
  Call(Operand(target), cond, r1, r2);
}


void MacroAssembler::Call(intptr_t target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2) {
  Call(Operand(target, rmode), cond, r1, r2);
}


void MacroAssembler::Call(byte* target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2) {
  ASSERT(!RelocInfo::IsCodeTarget(rmode));
  Call(reinterpret_cast<intptr_t>(target), rmode, cond, r1, r2);
}


void MacroAssembler::Call(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2) {
  ASSERT(RelocInfo::IsCodeTarget(rmode));
  Call(reinterpret_cast<intptr_t>(code.location()), rmode, cond, r1, r2);
}


void MacroAssembler::Ret(Condition cond, Register r1, const Operand& r2) {
  Jump(Operand(ra), cond, r1, r2);
}


void MacroAssembler::LoadRoot(Register destination,
                              Heap::RootListIndex index) {
  lw(destination, MemOperand(s6, index << kPointerSizeLog2));
}

void MacroAssembler::LoadRoot(Register destination,
                              Heap::RootListIndex index,
                              Condition cond,
                              Register src1, const Operand& src2) {
  Branch(NegateCondition(cond), 2, src1, src2);
  nop();
  lw(destination, MemOperand(s6, index << kPointerSizeLog2));
}


void MacroAssembler::RecordWrite(Register object, Register offset,
                                 Register scratch) {
  UNIMPLEMENTED_MIPS();
}


// ---------------------------------------------------------------------------
// Instruction macros

void MacroAssembler::Add(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    add(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      addi(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      add(rd, rs, at);
    }
  }
}


void MacroAssembler::Addu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    addu(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      addiu(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      addu(rd, rs, at);
    }
  }
}


void MacroAssembler::Mul(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    mul(rd, rs, rt.rm());
  } else {
    // li handles the relocation.
    ASSERT(!rs.is(at));
    li(at, rt);
    mul(rd, rs, at);
  }
}


void MacroAssembler::Mult(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    mult(rs, rt.rm());
  } else {
    // li handles the relocation.
    ASSERT(!rs.is(at));
    li(at, rt);
    mult(rs, at);
  }
}


void MacroAssembler::Multu(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    multu(rs, rt.rm());
  } else {
    // li handles the relocation.
    ASSERT(!rs.is(at));
    li(at, rt);
    multu(rs, at);
  }
}


void MacroAssembler::Div(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    div(rs, rt.rm());
  } else {
    // li handles the relocation.
    ASSERT(!rs.is(at));
    li(at, rt);
    div(rs, at);
  }
}


void MacroAssembler::Divu(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    divu(rs, rt.rm());
  } else {
    // li handles the relocation.
    ASSERT(!rs.is(at));
    li(at, rt);
    divu(rs, at);
  }
}


void MacroAssembler::And(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    and_(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      andi(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      and_(rd, rs, at);
    }
  }
}


void MacroAssembler::Or(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    or_(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      ori(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      or_(rd, rs, at);
    }
  }
}


void MacroAssembler::Xor(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    xor_(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      xori(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      xor_(rd, rs, at);
    }
  }
}


void MacroAssembler::Nor(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    nor(rd, rs, rt.rm());
  } else {
    // li handles the relocation.
    ASSERT(!rs.is(at));
    li(at, rt);
    nor(rd, rs, at);
  }
}


void MacroAssembler::Slt(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    slt(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      slti(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      slt(rd, rs, at);
    }
  }
}


void MacroAssembler::Sltu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    sltu(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      sltiu(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      sltu(rd, rs, at);
    }
  }
}


//------------Pseudo-instructions-------------

void MacroAssembler::movn(Register rd, Register rt) {
  addiu(at, zero_reg, -1);  // Fill at with ones.
  xor_(rd, rt, at);
}


// load wartd in a register
void MacroAssembler::li(Register rd, Operand j, bool gen2instr) {
  ASSERT(!j.is_reg());

  if (!MustUseAt(j.rmode_) && !gen2instr) {
    // Normal load of an immediate value which does not need Relocation Info.
    if (is_int16(j.imm32_)) {
      addiu(rd, zero_reg, j.imm32_);
    } else if (!(j.imm32_ & HIMask)) {
      ori(rd, zero_reg, j.imm32_);
    } else if (!(j.imm32_ & LOMask)) {
      lui(rd, (HIMask & j.imm32_) >> 16);
    } else {
      lui(rd, (HIMask & j.imm32_) >> 16);
      ori(rd, rd, (LOMask & j.imm32_));
    }
  } else if (MustUseAt(j.rmode_) || gen2instr) {
    if (MustUseAt(j.rmode_)) {
      RecordRelocInfo(j.rmode_, j.imm32_);
    }
    // We need always the same number of instructions as we may need to patch
    // this code to load another value which may need 2 instructions to load.
    if (is_int16(j.imm32_)) {
      nop();
      addiu(rd, zero_reg, j.imm32_);
    } else if (!(j.imm32_ & HIMask)) {
      nop();
      ori(rd, zero_reg, j.imm32_);
    } else if (!(j.imm32_ & LOMask)) {
      nop();
      lui(rd, (HIMask & j.imm32_) >> 16);
    } else {
      lui(rd, (HIMask & j.imm32_) >> 16);
      ori(rd, rd, (LOMask & j.imm32_));
    }
  }
}


// Exception-generating instructions and debugging support
void MacroAssembler::stop(const char* msg) {
  // TO_UPGRADE: Just a break for now. Maybe we could upgrade it.
  // We use the 0x54321 value to be able to find it easily when reading memory.
  break_(0x54321);
}


void MacroAssembler::MultiPush(RegList regs) {
  int16_t NumSaved = 0;
  int16_t NumToPush = NumberOfBitsSet(regs);

  addiu(sp, sp, -4 * NumToPush);
  for (int16_t i = kNumRegisters; i > 0; i--) {
    if ((regs & (1 << i)) != 0) {
      sw(ToRegister(i), MemOperand(sp, 4 * (NumToPush - ++NumSaved)));
    }
  }
}


void MacroAssembler::MultiPushReversed(RegList regs) {
  int16_t NumSaved = 0;
  int16_t NumToPush = NumberOfBitsSet(regs);

  addiu(sp, sp, -4 * NumToPush);
  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      sw(ToRegister(i), MemOperand(sp, 4 * (NumToPush - ++NumSaved)));
    }
  }
}


void MacroAssembler::MultiPop(RegList regs) {
  int16_t NumSaved = 0;

  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      lw(ToRegister(i), MemOperand(sp, 4 * (NumSaved++)));
    }
  }
  addiu(sp, sp, 4 * NumSaved);
}


void MacroAssembler::MultiPopReversed(RegList regs) {
  int16_t NumSaved = 0;

  for (int16_t i = kNumRegisters; i > 0; i--) {
    if ((regs & (1 << i)) != 0) {
      lw(ToRegister(i), MemOperand(sp, 4 * (NumSaved++)));
    }
  }
  addiu(sp, sp, 4 * NumSaved);
}


// Emulated condtional branches do not emit a nop in the branch delay slot.

// Trashes the at register if no scratch register is provided.
void MacroAssembler::Branch(Condition cond, int16_t offset, Register rs,
                            const Operand& rt, Register scratch) {
  Register r2;
  if (rt.is_reg()) {
    // We don't want any other register but scratch clobbered.
    ASSERT(!scratch.is(rs) && !scratch.is(rt.rm_));
    r2 = rt.rm_;
  } else if (cond != cc_always) {
    // We don't want any other register but scratch clobbered.
    ASSERT(!scratch.is(rs));
    r2 = scratch;
    li(r2, rt);
  }

  switch (cond) {
    case cc_always:
      b(offset);
      break;
    case eq:
      beq(rs, r2, offset);
      break;
    case ne:
      bne(rs, r2, offset);
      break;

      // Signed comparison
    case greater:
      slt(scratch, r2, rs);
      bne(scratch, zero_reg, offset);
      break;
    case greater_equal:
      slt(scratch, rs, r2);
      beq(scratch, zero_reg, offset);
      break;
    case less:
      slt(scratch, rs, r2);
      bne(scratch, zero_reg, offset);
      break;
    case less_equal:
      slt(scratch, r2, rs);
      beq(scratch, zero_reg, offset);
      break;

      // Unsigned comparison.
    case Ugreater:
      sltu(scratch, r2, rs);
      bne(scratch, zero_reg, offset);
      break;
    case Ugreater_equal:
      sltu(scratch, rs, r2);
      beq(scratch, zero_reg, offset);
      break;
    case Uless:
      sltu(scratch, rs, r2);
      bne(scratch, zero_reg, offset);
      break;
    case Uless_equal:
      sltu(scratch, r2, rs);
      beq(scratch, zero_reg, offset);
      break;

    default:
      UNREACHABLE();
  }
}


void MacroAssembler::Branch(Condition cond,  Label* L, Register rs,
                            const Operand& rt, Register scratch) {
  Register r2;
  if (rt.is_reg()) {
    r2 = rt.rm_;
  } else if (cond != cc_always) {
    r2 = scratch;
    li(r2, rt);
  }

  // We use branch_offset as an argument for the branch instructions to be sure
  // it is called just before generating the branch instruction, as needed.

  switch (cond) {
    case cc_always:
      b(shifted_branch_offset(L, false));
      break;
    case eq:
      beq(rs, r2, shifted_branch_offset(L, false));
      break;
    case ne:
      bne(rs, r2, shifted_branch_offset(L, false));
      break;

    // Signed comparison
    case greater:
      slt(scratch, r2, rs);
      bne(scratch, zero_reg, shifted_branch_offset(L, false));
      break;
    case greater_equal:
      slt(scratch, rs, r2);
      beq(scratch, zero_reg, shifted_branch_offset(L, false));
      break;
    case less:
      slt(scratch, rs, r2);
      bne(scratch, zero_reg, shifted_branch_offset(L, false));
      break;
    case less_equal:
      slt(scratch, r2, rs);
      beq(scratch, zero_reg, shifted_branch_offset(L, false));
      break;

    // Unsigned comparison.
    case Ugreater:
      sltu(scratch, r2, rs);
      bne(scratch, zero_reg, shifted_branch_offset(L, false));
      break;
    case Ugreater_equal:
      sltu(scratch, rs, r2);
      beq(scratch, zero_reg, shifted_branch_offset(L, false));
      break;
    case Uless:
      sltu(scratch, rs, r2);
      bne(scratch, zero_reg, shifted_branch_offset(L, false));
      break;
    case Uless_equal:
      sltu(scratch, r2, rs);
      beq(scratch, zero_reg, shifted_branch_offset(L, false));
      break;

    default:
      UNREACHABLE();
  }
}


// Trashes the at register if no scratch register is provided.
// We need to use a bgezal or bltzal, but they can't be used directly with the
// slt instructions. We could use sub or add instead but we would miss overflow
// cases, so we keep slt and add an intermediate third instruction.
void MacroAssembler::BranchAndLink(Condition cond, int16_t offset, Register rs,
                                   const Operand& rt, Register scratch) {
  Register r2;
  if (rt.is_reg()) {
    r2 = rt.rm_;
  } else if (cond != cc_always) {
    r2 = scratch;
    li(r2, rt);
  }

  switch (cond) {
    case cc_always:
      bal(offset);
      break;
    case eq:
      bne(rs, r2, 2);
      nop();
      bal(offset);
      break;
    case ne:
      beq(rs, r2, 2);
      nop();
      bal(offset);
      break;

    // Signed comparison
    case greater:
      slt(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bgezal(scratch, offset);
      break;
    case greater_equal:
      slt(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bltzal(scratch, offset);
      break;
    case less:
      slt(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bgezal(scratch, offset);
      break;
    case less_equal:
      slt(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bltzal(scratch, offset);
      break;

    // Unsigned comparison.
    case Ugreater:
      sltu(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bgezal(scratch, offset);
      break;
    case Ugreater_equal:
      sltu(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bltzal(scratch, offset);
      break;
    case Uless:
      sltu(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bgezal(scratch, offset);
      break;
    case Uless_equal:
      sltu(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bltzal(scratch, offset);
      break;

    default:
      UNREACHABLE();
  }
}


void MacroAssembler::BranchAndLink(Condition cond, Label* L, Register rs,
                                   const Operand& rt, Register scratch) {
  Register r2;
  if (rt.is_reg()) {
    r2 = rt.rm_;
  } else if (cond != cc_always) {
    r2 = scratch;
    li(r2, rt);
  }

  switch (cond) {
    case cc_always:
      bal(shifted_branch_offset(L, false));
      break;
    case eq:
      bne(rs, r2, 2);
      nop();
      bal(shifted_branch_offset(L, false));
      break;
    case ne:
      beq(rs, r2, 2);
      nop();
      bal(shifted_branch_offset(L, false));
      break;

    // Signed comparison
    case greater:
      slt(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bgezal(scratch, shifted_branch_offset(L, false));
      break;
    case greater_equal:
      slt(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bltzal(scratch, shifted_branch_offset(L, false));
      break;
    case less:
      slt(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bgezal(scratch, shifted_branch_offset(L, false));
      break;
    case less_equal:
      slt(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bltzal(scratch, shifted_branch_offset(L, false));
      break;

    // Unsigned comparison.
    case Ugreater:
      sltu(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bgezal(scratch, shifted_branch_offset(L, false));
      break;
    case Ugreater_equal:
      sltu(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bltzal(scratch, shifted_branch_offset(L, false));
      break;
    case Uless:
      sltu(scratch, rs, r2);
      addiu(scratch, scratch, -1);
      bgezal(scratch, shifted_branch_offset(L, false));
      break;
    case Uless_equal:
      sltu(scratch, r2, rs);
      addiu(scratch, scratch, -1);
      bltzal(scratch, shifted_branch_offset(L, false));
      break;

    default:
      UNREACHABLE();
  }
}


void MacroAssembler::Jump(const Operand& target,
                          Condition cond, Register rs, const Operand& rt) {
  if (target.is_reg()) {
    if (cond == cc_always) {
      jr(target.rm());
    } else {
      Branch(NegateCondition(cond), 2, rs, rt);
      nop();
      jr(target.rm());
    }
  } else {    // !target.is_reg()
    if (!MustUseAt(target.rmode_)) {
      if (cond == cc_always) {
        j(target.imm32_);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        nop();
        j(target.imm32_);  // will generate only one instruction.
      }
    } else {  // MustUseAt(target)
      li(at, target);
      if (cond == cc_always) {
        jr(at);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        nop();
        jr(at);  // will generate only one instruction.
      }
    }
  }
}


void MacroAssembler::Call(const Operand& target,
                          Condition cond, Register rs, const Operand& rt) {
  if (target.is_reg()) {
    if (cond == cc_always) {
      jalr(target.rm());
    } else {
      Branch(NegateCondition(cond), 2, rs, rt);
      nop();
      jalr(target.rm());
    }
  } else {    // !target.is_reg()
    if (!MustUseAt(target.rmode_)) {
      if (cond == cc_always) {
        jal(target.imm32_);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        nop();
        jal(target.imm32_);  // will generate only one instruction.
      }
    } else {  // MustUseAt(target)
      li(at, target);
      if (cond == cc_always) {
        jalr(at);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        nop();
        jalr(at);  // will generate only one instruction.
      }
    }
  }
}

void MacroAssembler::StackLimitCheck(Label* on_stack_overflow) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::Drop(int count, Condition cond) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::Call(Label* target) {
  UNIMPLEMENTED_MIPS();
}


#ifdef ENABLE_DEBUGGER_SUPPORT
// ---------------------------------------------------------------------------
// Debugger Support

void MacroAssembler::SaveRegistersToMemory(RegList regs) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::RestoreRegistersFromMemory(RegList regs) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::CopyRegistersFromMemoryToStack(Register base,
                                                    RegList regs) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::CopyRegistersFromStackToMemory(Register base,
                                                    Register scratch,
                                                    RegList regs) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::DebugBreak() {
  UNIMPLEMENTED_MIPS();
}
#endif

// ---------------------------------------------------------------------------
// Exception handling

void MacroAssembler::PushTryHandler(CodeLocation try_location,
                                    HandlerType type) {
  // Adjust this code if not the case.
  ASSERT(StackHandlerConstants::kSize == 4 * kPointerSize);
  // The return address is passed in register ra.
  if (try_location == IN_JAVASCRIPT) {
    if (type == TRY_CATCH_HANDLER) {
      li(t0, Operand(StackHandler::TRY_CATCH));
    } else {
      li(t0, Operand(StackHandler::TRY_FINALLY));
    }
    ASSERT(StackHandlerConstants::kStateOffset == 1 * kPointerSize
           && StackHandlerConstants::kFPOffset == 2 * kPointerSize
           && StackHandlerConstants::kPCOffset == 3 * kPointerSize
           && StackHandlerConstants::kNextOffset == 0 * kPointerSize);
    // Save the current handler as the next handler.
    li(t2, Operand(ExternalReference(Top::k_handler_address)));
    lw(t1, MemOperand(t2));
   
    addiu(sp, sp, -StackHandlerConstants::kSize);
    sw(ra, MemOperand(sp, 12));
    sw(fp, MemOperand(sp, 8));
    sw(t0, MemOperand(sp, 4));
    sw(t1, MemOperand(sp, 0));
    
    // Link this handler as the new current one.
    sw(sp, MemOperand(t2));

  } else {
    // Must preserve a0-a3, and s0 (argv).
    ASSERT(try_location == IN_JS_ENTRY);
    ASSERT(StackHandlerConstants::kStateOffset == 1 * kPointerSize
           && StackHandlerConstants::kFPOffset == 2 * kPointerSize
           && StackHandlerConstants::kPCOffset == 3 * kPointerSize
           && StackHandlerConstants::kNextOffset == 0 * kPointerSize);

    // The frame pointer does not point to a JS frame so we save NULL
    // for fp. We expect the code throwing an exception to check fp
    // before dereferencing it to restore the context.
    li(t0, Operand(StackHandler::ENTRY));

    // Save the current handler as the next handler.
    li(t2, Operand(ExternalReference(Top::k_handler_address)));
    lw(t1, MemOperand(t2));

    addiu(sp, sp, -StackHandlerConstants::kSize);
    sw(ra, MemOperand(sp, 12));
    sw(zero_reg, MemOperand(sp, 8));
    sw(t0, MemOperand(sp, 4));
    sw(t1, MemOperand(sp, 0));

    // Link this handler as the new current one.
    sw(sp, MemOperand(t2));
  }
}


void MacroAssembler::PopTryHandler() {
  UNIMPLEMENTED_MIPS();
}



// -----------------------------------------------------------------------------
// Activation frames

void MacroAssembler::SetupAlignedCall(Register scratch, int arg_count) {
  andi(scratch, sp, 7);
  // Store sp on the stack. If necessary allocate some extra space to preserve
  // arguments' 8-byte alignment.
    sw(sp, MemOperand(sp, -4));
    sw(sp, MemOperand(sp, -8));
  if(((arg_count + 1) % 2) == 0) {
    beq(scratch, zero_reg, 2);
  } else {  // ((arg_count + 1) % 2) == 1
    bne(scratch, zero_reg, 2);
  }
    addiu(sp, sp, -4);
    addiu(sp, sp, -4);
}


void MacroAssembler::ReturnFromAlignedCall() {
  lw(sp, MemOperand(sp));
}


// -----------------------------------------------------------------------------
// JavaScript invokes

void MacroAssembler::InvokePrologue(const ParameterCount& expected,
                                    const ParameterCount& actual,
                                    Handle<Code> code_constant,
                                    Register code_reg,
                                    Label* done,
                                    InvokeFlag flag) {

  bool definitely_matches = false;
  Label regular_invoke;

  // Check whether the expected and actual arguments count match. If not,
  // setup registers according to contract with ArgumentsAdaptorTrampoline:
  //  a0: actual arguments count
  //  a1: function (passed through to callee)
  //  a2: expected arguments count
  //  a3: callee code entry

  // The code below is made a lot easier because the calling code already sets
  // up actual and expected registers according to the contract if values are
  // passed in registers.
  ASSERT(actual.is_immediate() || actual.reg().is(a0));
  ASSERT(expected.is_immediate() || expected.reg().is(a2));
  ASSERT((!code_constant.is_null() && code_reg.is(no_reg)) || code_reg.is(a3));

  if (expected.is_immediate()) {
    ASSERT(actual.is_immediate());
    if (expected.immediate() == actual.immediate()) {
      definitely_matches = true;
    } else {
      li(a0, Operand(actual.immediate()));
      const int sentinel = SharedFunctionInfo::kDontAdaptArgumentsSentinel;
      if (expected.immediate() == sentinel) {
        // Don't worry about adapting arguments for builtins that
        // don't want that done. Skip adaption code by making it look
        // like we have a match between expected and actual number of
        // arguments.
        definitely_matches = true;
      } else {
        li(a2, Operand(expected.immediate()));
      }
    }
  } else if (actual.is_immediate()) {
    Branch(eq, &regular_invoke, expected.reg(), Operand(actual.immediate()));
    nop();
    li(a0, Operand(actual.immediate()));
  } else {
    Branch(eq, &regular_invoke, expected.reg(), Operand(actual.reg()));
    nop();
  }

  if (!definitely_matches) {
    if (!code_constant.is_null()) {
      li(a3, Operand(code_constant));
      addiu(a3, a3, Code::kHeaderSize - kHeapObjectTag);
    }

    ExternalReference adaptor(Builtins::ArgumentsAdaptorTrampoline);
    if (flag == CALL_FUNCTION) {
      CallBuiltin(adaptor);
      nop();
      b(done);
      nop();
    } else {
      JumpToBuiltin(adaptor);
      nop();
    }
    bind(&regular_invoke);
  }
}

void MacroAssembler::InvokeCode(Register code,
                                const ParameterCount& expected,
                                const ParameterCount& actual,
                                InvokeFlag flag) {
  Label done;

  InvokePrologue(expected, actual, Handle<Code>::null(), code,
                      &done, flag);
  if (flag == CALL_FUNCTION) {
    Call(code);
  } else {
    ASSERT(flag == JUMP_FUNCTION);
    Jump(code);
  }
  // Continue here if InvokePrologue does handle the invocation due to
  // mismatched parameter counts.
  bind(&done);
}


void MacroAssembler::InvokeCode(Handle<Code> code,
                                const ParameterCount& expected,
                                const ParameterCount& actual,
                                RelocInfo::Mode rmode,
                                InvokeFlag flag) {
  Label done;

  InvokePrologue(expected, actual, code, no_reg,
                      &done, flag);
  if (flag == CALL_FUNCTION) {
    Call(code, rmode);
  } else {
    Jump(code, rmode);
  }
  // Continue here if InvokePrologue does handle the invocation due to
  // mismatched parameter counts.
  bind(&done);
}


void MacroAssembler::InvokeFunction(Register function,
                                    const ParameterCount& actual,
                                    InvokeFlag flag) {
  // Contract with called JS functions requires that function is passed in a1.
  ASSERT(function.is(a1));
  Register expected_reg = a2;
  Register code_reg = a3;

  lw(code_reg, FieldMemOperand(a1, JSFunction::kSharedFunctionInfoOffset));
  lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));
  lw(expected_reg,
      FieldMemOperand(code_reg,
                      SharedFunctionInfo::kFormalParameterCountOffset));
  lw(code_reg,
      MemOperand(code_reg, SharedFunctionInfo::kCodeOffset - kHeapObjectTag));
  addiu(code_reg, code_reg, Code::kHeaderSize - kHeapObjectTag);

  ParameterCount expected(expected_reg);
  InvokeCode(code_reg, expected, actual, flag);
  // We want the branch delay slot to be free.
}


// ---------------------------------------------------------------------------
// Support functions.

  void MacroAssembler::CallBuiltin(ExternalReference builtin_entry) {
    // Load builtin address.
    li(t9, Operand(builtin_entry));
    lw(t9, MemOperand(t9));  // deref address
    addiu(t9, t9, Code::kHeaderSize - kHeapObjectTag);
    // Call and allocate arguments slots.
    jalr(t9);
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::CallBuiltin(Register target) {
    // t9 already holds target address.
    // Call and allocate arguments slots.
    jalr(t9);
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::JumpToBuiltin(ExternalReference builtin_entry) {
    // Load builtin address.
    li(t9, Operand(builtin_entry));
    lw(t9, MemOperand(t9));  // deref address
    addiu(t9, t9, Code::kHeaderSize - kHeapObjectTag);
    // Call and allocate arguments slots.
    jr(t9);
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::JumpToBuiltin(Register target) {
    // t9 already holds target address.
    // Call and allocate arguments slots.
    jr(t9);
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  }


// -----------------------------------------------------------------------------
// Runtime calls

void MacroAssembler::CallStub(CodeStub* stub, Condition cond,
                              Register r1, const Operand& r2) {
  ASSERT(allow_stub_calls());  // stub calls are not allowed in some stubs
  Call(stub->GetCode(), RelocInfo::CODE_TARGET, cond, r1, r2);
}


void MacroAssembler::StubReturn(int argc) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::IllegalOperation(int num_arguments) {
  if (num_arguments > 0) {
    addiu(sp, sp, num_arguments * kPointerSize);
  }
  LoadRoot(v0, Heap::kUndefinedValueRootIndex);
}


void MacroAssembler::CallRuntime(Runtime::Function* f, int num_arguments) {
  // All parameters are on the stack.  r0->v0 has the return value after call.

  // If the expected number of arguments of the runtime function is
  // constant, we check that the actual number of arguments match the
  // expectation.
  if (f->nargs >= 0 && f->nargs != num_arguments) {
    IllegalOperation(num_arguments);
    return;
  }

  // TODO(1236192): Most runtime routines don't need the number of
  // arguments passed in because it is constant. At some point we
  // should remove this need and make the runtime routine entry code
  // smarter.
  li(a0, num_arguments);
  li(a1, Operand(ExternalReference(f)));
  CEntryStub stub(1);
  CallStub(&stub);
}


void MacroAssembler::CallRuntime(Runtime::FunctionId fid, int num_arguments) {
  CallRuntime(Runtime::FunctionForId(fid), num_arguments);
}


void MacroAssembler::TailCallExternalReference(const ExternalReference& ext,
                                               int num_arguments,
                                               int result_size) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::TailCallRuntime(Runtime::FunctionId fid,
                                     int num_arguments,
                                     int result_size) {
  TailCallExternalReference(ExternalReference(fid), num_arguments, result_size);
}


void MacroAssembler::JumpToExternalReference(const ExternalReference& builtin) {
  UNIMPLEMENTED_MIPS();
}


Handle<Code> MacroAssembler::ResolveBuiltin(Builtins::JavaScript id,
                                            bool* resolved) {
  UNIMPLEMENTED_MIPS();
  return Handle<Code>(reinterpret_cast<Code*>(NULL));   // UNIMPLEMENTED RETURN
}


void MacroAssembler::InvokeBuiltin(Builtins::JavaScript id,
                                   InvokeJSFlags flags) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::GetBuiltinEntry(Register target, Builtins::JavaScript id) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::SetCounter(StatsCounter* counter, int value,
                                Register scratch1, Register scratch2) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::IncrementCounter(StatsCounter* counter, int value,
                                      Register scratch1, Register scratch2) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::DecrementCounter(StatsCounter* counter, int value,
                                      Register scratch1, Register scratch2) {
  UNIMPLEMENTED_MIPS();
}


// -----------------------------------------------------------------------------
// Debugging

void MacroAssembler::Assert(Condition cc, const char* msg,
                            Register rs, Operand rt) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::Check(Condition cc, const char* msg,
                           Register rs, Operand rt) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::Abort(const char* msg) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::EnterFrame(StackFrame::Type type) {
  addiu(sp, sp, -5 * kPointerSize);
  li(t0, Operand(Smi::FromInt(type)));
  li(t1, Operand(CodeObject()));
  sw(ra, MemOperand(sp, 4 * kPointerSize));
  sw(fp, MemOperand(sp, 3 * kPointerSize));
  sw(cp, MemOperand(sp, 2 * kPointerSize));
  sw(t0, MemOperand(sp, 1 * kPointerSize));
  sw(t1, MemOperand(sp, 0 * kPointerSize));
  addiu(fp, sp, 3 * kPointerSize);
}


void MacroAssembler::LeaveFrame(StackFrame::Type type) {
  mov(sp, fp);
  lw(fp, MemOperand(sp, 0 * kPointerSize));
  lw(ra, MemOperand(sp, 1 * kPointerSize));
  addiu(sp, sp, 2 * kPointerSize);
}

} }  // namespace v8::internal

