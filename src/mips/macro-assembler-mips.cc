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
                          Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  Jump(Operand(target), cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Jump(intptr_t target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  Jump(Operand(target, rmode), cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Jump(byte* target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  ASSERT(!RelocInfo::IsCodeTarget(rmode));
  Jump(reinterpret_cast<intptr_t>(target), rmode, cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Jump(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  ASSERT(RelocInfo::IsCodeTarget(rmode));
  Jump(reinterpret_cast<intptr_t>(code.location()), rmode, cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Call(Register target,
                          Condition cond, Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  Call(Operand(target), cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Call(intptr_t target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  Call(Operand(target, rmode), cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Call(byte* target, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  ASSERT(!RelocInfo::IsCodeTarget(rmode));
  Call(reinterpret_cast<intptr_t>(target), rmode, cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Call(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond, Register r1, const Operand& r2,
                          bool ProtectBranchDelaySlot) {
  ASSERT(RelocInfo::IsCodeTarget(rmode));
  Call(reinterpret_cast<intptr_t>(code.location()), rmode, cond, r1, r2, ProtectBranchDelaySlot);
}


void MacroAssembler::Ret(Condition cond, Register r1, const Operand& r2,
    bool ProtectBranchDelaySlot) {
  Jump(Operand(ra), cond, r1, r2, ProtectBranchDelaySlot);
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
  lw(destination, MemOperand(s6, index << kPointerSizeLog2));
}


void MacroAssembler::RecordWrite(Register object,
                                 Register offset,
                                 Register scratch) {
  UNIMPLEMENTED_MIPS();
}


// -----------------------------------------------------------------------------
// Allocation support


Register MacroAssembler::CheckMaps(JSObject* object, Register object_reg,
                                   JSObject* holder, Register holder_reg,
                                   Register scratch,
                                   Label* miss) {
  // Make sure there's no overlap between scratch and the other
  // registers.
  ASSERT(!scratch.is(object_reg) && !scratch.is(holder_reg));

  // Keep track of the current object in register reg.
  Register reg = object_reg;
  int depth = 1;

  // Check the maps in the prototype chain.
  // Traverse the prototype chain from the object and do map checks.
  while (object != holder) {
    depth++;

    // Only global objects and objects that do not require access
    // checks are allowed in stubs.
    ASSERT(object->IsJSGlobalProxy() || !object->IsAccessCheckNeeded());

    // Get the map of the current object.
    lw(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));

    // Branch on the result of the map check.
    Branch(ne, miss, scratch, Operand(Handle<Map>(object->map())));

    // Check access rights to the global object.  This has to happen
    // after the map check so that we know that the object is
    // actually a global object.
    if (object->IsJSGlobalProxy()) {
      CheckAccessGlobalProxy(reg, scratch, miss);
      // Restore scratch register to be the map of the object.  In the
      // new space case below, we load the prototype from the map in
      // the scratch register.
      lw(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));
    }

    reg = holder_reg;  // From now the object is in holder_reg.
    JSObject* prototype = JSObject::cast(object->GetPrototype());
    if (Heap::InNewSpace(prototype)) {
      // The prototype is in new space; we cannot store a reference
      // to it in the code. Load it from the map.
      lw(reg, FieldMemOperand(scratch, Map::kPrototypeOffset));
    } else {
      // The prototype is in old space; load it directly.
      li(reg, Operand(Handle<JSObject>(prototype)));
    }

    // Go to the next object in the prototype chain.
    object = prototype;
  }

  // Check the holder map.
  lw(scratch, FieldMemOperand(reg, HeapObject::kMapOffset));
  Branch(ne, miss, scratch, Operand(Handle<Map>(object->map())));

  // Log the check depth.
  LOG(IntEvent("check-maps-depth", depth));

  // Perform security check for access to the global object and return
  // the holder register.
  ASSERT(object == holder);
  ASSERT(object->IsJSGlobalProxy() || !object->IsAccessCheckNeeded());
  if (object->IsJSGlobalProxy()) {
    CheckAccessGlobalProxy(reg, scratch, miss);
  }
  return reg;
}


void MacroAssembler::CheckAccessGlobalProxy(Register holder_reg,
                                            Register scratch,
                                            Label* miss) {
  Label same_contexts;

  ASSERT(!holder_reg.is(scratch));
  ASSERT(!holder_reg.is(at));
  ASSERT(!scratch.is(at));

  // Load current lexical context from the stack frame.
  lw(scratch, MemOperand(fp, StandardFrameConstants::kContextOffset));
  // In debug mode, make sure the lexical context is set.
#ifdef DEBUG
  Check(ne, "we should not have an empty lexical context",
      scratch, Operand(zero_reg));
#endif

  // Load the global context of the current context.
  int offset = Context::kHeaderSize + Context::GLOBAL_INDEX * kPointerSize;
  lw(scratch, FieldMemOperand(scratch, offset));
  lw(scratch, FieldMemOperand(scratch, GlobalObject::kGlobalContextOffset));

  // Check the context is a global context.
  if (FLAG_debug_code) {
    // TODO(119): Avoid push(holder_reg)/pop(holder_reg).
    // Cannot use at as a temporary in this verification code. Due to the fact
    // that at is clobbered as part of cmp with an object Operand.
    Push(holder_reg);  // Temporarily save holder on the stack.
    // Read the first word and compare to the global_context_map.
    lw(holder_reg, FieldMemOperand(scratch, HeapObject::kMapOffset));
    LoadRoot(at, Heap::kGlobalContextMapRootIndex);
    Check(eq, "JSGlobalObject::global_context should be a global context.",
          holder_reg, Operand(at));
    Pop(holder_reg);  // Restore holder.
  }

  // Check if both contexts are the same.
  lw(at, FieldMemOperand(holder_reg, JSGlobalProxy::kContextOffset));
  Branch(eq, &same_contexts, scratch, Operand(at));

  // Check the context is a global context.
  if (FLAG_debug_code) {
    // TODO(119): Avoid push(holder_reg)/pop(holder_reg).
    // Cannot use ip as a temporary in this verification code. Due to the fact
    // that ip is clobbered as part of cmp with an object Operand.
    push(holder_reg);  // Temporarily save holder on the stack.
    mov(holder_reg, at);  // Move ip to its holding place.
    LoadRoot(at, Heap::kNullValueRootIndex);
    Check(ne, "JSGlobalProxy::context() should not be null.",
          holder_reg, Operand(at));

    lw(holder_reg, FieldMemOperand(holder_reg, HeapObject::kMapOffset));
    LoadRoot(at, Heap::kGlobalContextMapRootIndex);
    Check(eq, "JSGlobalObject::global_context should be a global context.",
          holder_reg, Operand(at));
    // Restore at is not needed. at is reloaded below.
    Pop(holder_reg);  // Restore holder.
    // Restore at to holder's context.
    lw(at, FieldMemOperand(holder_reg, JSGlobalProxy::kContextOffset));
  }

  // Check that the security token in the calling global object is
  // compatible with the security token in the receiving global
  // object.
  int token_offset = Context::kHeaderSize +
                     Context::SECURITY_TOKEN_INDEX * kPointerSize;

  lw(scratch, FieldMemOperand(scratch, token_offset));
  lw(at, FieldMemOperand(at, token_offset));
  Branch(ne, miss, scratch, Operand(at));

  bind(&same_contexts);
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


void MacroAssembler::Subu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    subu(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseAt(rt.rmode_)) {
      addiu(rd, rs, -rt.imm32_);  // No subiu instr, use addiu(x, y, -imm).
    } else {
      // li handles the relocation.
      ASSERT(!rs.is(at));
      li(at, rt);
      subu(rd, rs, at);
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
                            const Operand& rt, Register scratch,
                            bool ProtectBranchDelaySlot) {
  Register r2 = no_reg;
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
  // Emit a nop in the branch delay slot if required.
  if (ProtectBranchDelaySlot)
    nop();
}


void MacroAssembler::Branch(Condition cond,  Label* L, Register rs,
                            const Operand& rt, Register scratch,
                            bool ProtectBranchDelaySlot) {
  Register r2 = no_reg;
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
  // Emit a nop in the branch delay slot if required.
  if (ProtectBranchDelaySlot)
    nop();
}


// Trashes the at register if no scratch register is provided.
// We need to use a bgezal or bltzal, but they can't be used directly with the
// slt instructions. We could use sub or add instead but we would miss overflow
// cases, so we keep slt and add an intermediate third instruction.
void MacroAssembler::BranchAndLink(Condition cond, int16_t offset, Register rs,
                                   const Operand& rt, Register scratch,
                                   bool ProtectBranchDelaySlot) {
  Register r2 = no_reg;
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
  // Emit a nop in the branch delay slot if required.
  if (ProtectBranchDelaySlot)
    nop();
}


void MacroAssembler::BranchAndLink(Condition cond, Label* L, Register rs,
                                   const Operand& rt, Register scratch,
                                   bool ProtectBranchDelaySlot) {
  Register r2 = no_reg;
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
  // Emit a nop in the branch delay slot if required.
  if (ProtectBranchDelaySlot)
    nop();
}


void MacroAssembler::Jump(const Operand& target,
                          Condition cond, Register rs, const Operand& rt,
                          bool ProtectBranchDelaySlot) {
  if (target.is_reg()) {
    if (cond == cc_always) {
      jr(target.rm());
    } else {
      Branch(NegateCondition(cond), 2, rs, rt);
      jr(target.rm());
    }
  } else {    // !target.is_reg()
    if (!MustUseAt(target.rmode_)) {
      if (cond == cc_always) {
        j(target.imm32_);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        j(target.imm32_);  // Will generate only one instruction.
      }
    } else {  // MustUseAt(target)
      li(at, target);
      if (cond == cc_always) {
        jr(at);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        jr(at);  // Will generate only one instruction.
      }
    }
  }
  // Emit a nop in the branch delay slot if required.
  if (ProtectBranchDelaySlot)
    nop();
}


void MacroAssembler::Call(const Operand& target,
                          Condition cond, Register rs, const Operand& rt,
                          bool ProtectBranchDelaySlot) {
  if (target.is_reg()) {
    if (cond == cc_always) {
      jalr(target.rm());
    } else {
      Branch(NegateCondition(cond), 2, rs, rt);
      jalr(target.rm());
    }
  } else {    // !target.is_reg()
    if (!MustUseAt(target.rmode_)) {
      if (cond == cc_always) {
        jal(target.imm32_);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        jal(target.imm32_);  // Will generate only one instruction.
      }
    } else {  // MustUseAt(target)
      li(at, target);
      if (cond == cc_always) {
        jalr(at);
      } else {
        Branch(NegateCondition(cond), 2, rs, rt);
        jalr(at);  // Will generate only one instruction.
      }
    }
  }
  // Emit a nop in the branch delay slot if required.
  if (ProtectBranchDelaySlot)
    nop();
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
    LoadExternalReference(t2, ExternalReference(Top::k_handler_address));
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
    LoadExternalReference(t2, ExternalReference(Top::k_handler_address));
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


void MacroAssembler::AllocateInNewSpace(int object_size,
                                              Register result,
                                              Register scratch1,
                                              Register scratch2,
                                              Label* gc_required,
                                              AllocationFlags flags) {
  ASSERT(!result.is(scratch1));
  ASSERT(!scratch1.is(scratch2));

  // Load address of new object into result and allocation top address into
  // scratch1.
  ExternalReference new_space_allocation_top =
      ExternalReference::new_space_allocation_top_address();
  li(scratch1, Operand(new_space_allocation_top));
  if ((flags & RESULT_CONTAINS_TOP) == 0) {
    lw(result, MemOperand(scratch1));
  } else {
#ifdef DEBUG
    // Assert that result actually contains top on entry. scratch2 is used
    // immediately below so this use of scratch2 does not cause difference with
    // respect to register content between debug and release mode.
    lw(scratch2, MemOperand(scratch1));
    Check(eq, "Unexpected allocation top", result, Operand(scratch2));
#endif
  }

  // Calculate new top and bail out if new space is exhausted. Use result
  // to calculate the new top.
  ExternalReference new_space_allocation_limit =
      ExternalReference::new_space_allocation_limit_address();
//  mov(scratch2, Operand(new_space_allocation_limit));
//  ldr(scratch2, MemOperand(scratch2));
//  add(result, result, Operand(object_size * kPointerSize));
//  cmp(result, Operand(scratch2));
//  b(hi, gc_required);
  li(scratch2, Operand(new_space_allocation_limit));
  lw(scratch2, MemOperand(scratch2));
  Addu(result, result, Operand(object_size * kPointerSize));
  Branch(Ugreater, gc_required, result, Operand(scratch2));
  nop(); // NOP_ADDED

  // Update allocation top. result temporarily holds the new top,
//  str(result, MemOperand(scratch1));
  sw(result, MemOperand(scratch1));

  // Tag and adjust back to start of new object.
  if ((flags & TAG_OBJECT) != 0) {
    Addu(result, result, Operand(-(object_size * kPointerSize) +
                                kHeapObjectTag));
  } else {
    Addu(result, result, Operand(-object_size * kPointerSize));
  }
}


void MacroAssembler::AllocateInNewSpace(Register object_size,
                                        Register result,
                                        Register scratch1,
                                        Register scratch2,
                                        Label* gc_required,
                                        AllocationFlags flags) {
  ASSERT(!result.is(scratch1));
  ASSERT(!scratch1.is(scratch2));

  // Load address of new object into result and allocation top address into
  // scratch1.
  ExternalReference new_space_allocation_top =
      ExternalReference::new_space_allocation_top_address();
//  mov(scratch1, Operand(new_space_allocation_top));
  li(scratch1, Operand(new_space_allocation_top));
  if ((flags & RESULT_CONTAINS_TOP) == 0) {
//    ldr(result, MemOperand(scratch1));
    lw(result, MemOperand(scratch1));
  } else {
#ifdef DEBUG
    // Assert that result actually contains top on entry. scratch2 is used
    // immediately below so this use of scratch2 does not cause difference with
    // respect to register content between debug and release mode.
//    ldr(scratch2, MemOperand(scratch1));
//    cmp(result, scratch2);
//    Check(eq, "Unexpected allocation top");
    lw(scratch2, MemOperand(scratch1));
    Check(eq, "Unexpected allocation top", result, Operand(scratch2));
#endif
  }

  // Calculate new top and bail out if new space is exhausted. Use result
  // to calculate the new top. Object size is in words so a shift is required to
  // get the number of bytes
  ExternalReference new_space_allocation_limit =
      ExternalReference::new_space_allocation_limit_address();
//  mov(scratch2, Operand(new_space_allocation_limit));
//  ldr(scratch2, MemOperand(scratch2));
//  add(result, result, Operand(object_size, LSL, kPointerSizeLog2));
//  cmp(result, Operand(scratch2));
//  b(hi, gc_required);
  li(scratch2, Operand(new_space_allocation_limit));
  lw(scratch2, MemOperand(scratch2));
  sll(ip, object_size, kPointerSizeLog2);
  Addu(result, result, Operand(ip));
  Branch(Ugreater, gc_required, result, Operand(scratch2));
  nop(); // NOP_ADDED

  // Update allocation top. result temporarily holds the new top,
//  str(result, MemOperand(scratch1));
  sw(result, MemOperand(scratch1));

  // Adjust back to start of new object.
//  sub(result, result, Operand(object_size, LSL, kPointerSizeLog2));
  Subu(result, result, Operand(ip));

  // Tag object if requested.
  if ((flags & TAG_OBJECT) != 0) {
//    add(result, result, Operand(kHeapObjectTag));
    Addu(result, result, Operand(kHeapObjectTag));
  }
}


void MacroAssembler::UndoAllocationInNewSpace(Register object,
                                              Register scratch) {
  ExternalReference new_space_allocation_top =
      ExternalReference::new_space_allocation_top_address();

  // Make sure the object has no tag before resetting top.
  And(object, object, Operand(~kHeapObjectTagMask));
#ifdef DEBUG
  // Check that the object un-allocated is below the current top.
//  mov(scratch, Operand(new_space_allocation_top));
//  ldr(scratch, MemOperand(scratch));
//  cmp(object, scratch);
//  Check(lt, "Undo allocation of non allocated memory");
  li(scratch, Operand(new_space_allocation_top));
  lw(scratch, MemOperand(scratch));
  Check(less, "Undo allocation of non allocated memory", object, Operand(scratch));
#endif
  // Write the address of the object to un-allocate as the current top.
  li(scratch, Operand(new_space_allocation_top));
  sw(object, MemOperand(scratch));
}




// -----------------------------------------------------------------------------
// Activation frames

void MacroAssembler::SetupAlignedCall(Register scratch, int arg_count) {
  Label extra_push, end;

  andi(scratch, sp, 7);

  // We check for args and receiver size on the stack, all of them word sized.
  // We add one for sp, that we also want to store on the stack.
  if (((arg_count + 1) % kPointerSizeLog2) == 0) {
    Branch(ne, &extra_push, at, Operand(zero_reg));
  } else {  // ((arg_count + 1) % 2) == 1
    Branch(eq, &extra_push, at, Operand(zero_reg));
  }

  // Save sp on the stack.
  mov(scratch, sp);
  Push(scratch);
  b(&end);

  // Align before saving sp on the stack.
  bind(&extra_push);
  mov(scratch, sp);
  addiu(sp, sp, -8);
  sw(scratch, MemOperand(sp));

  // The stack is aligned and sp is stored on the top.
  bind(&end);
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
    li(a0, Operand(actual.immediate()));
  } else {
    Branch(eq, &regular_invoke, expected.reg(), Operand(actual.reg()));
  }

  if (!definitely_matches) {
    if (!code_constant.is_null()) {
      li(a3, Operand(code_constant));
      addiu(a3, a3, Code::kHeaderSize - kHeapObjectTag);
    }

    ExternalReference adaptor(Builtins::ArgumentsAdaptorTrampoline);
    if (flag == CALL_FUNCTION) {
      CallBuiltin(adaptor);
      b(done);
      nop();
    } else {
      JumpToBuiltin(adaptor);
    }
    bind(&regular_invoke);
  }
}

void MacroAssembler::InvokeCode(Register code,
                                const ParameterCount& expected,
                                const ParameterCount& actual,
                                InvokeFlag flag) {
  Label done;

  InvokePrologue(expected, actual, Handle<Code>::null(), code, &done, flag);
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

  InvokePrologue(expected, actual, code, no_reg, &done, flag);
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
}


// ---------------------------------------------------------------------------
// Support functions.

  void MacroAssembler::GetObjectType(Register function,
                                     Register map,
                                     Register type_reg) {
    lw(map, FieldMemOperand(function, HeapObject::kMapOffset));
    lbu(type_reg, FieldMemOperand(map, Map::kInstanceTypeOffset));
  }


  void MacroAssembler::CallBuiltin(ExternalReference builtin_entry) {
    // Load builtin address.
    LoadExternalReference(t9, builtin_entry);
    lw(t9, MemOperand(t9));  // Deref address.
    addiu(t9, t9, Code::kHeaderSize - kHeapObjectTag);
    // Call and allocate arguments slots.
    jalr(t9);
    // Use the branch delay slot to allocated argument slots.
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
    addiu(sp, sp, StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::CallBuiltin(Register target) {
    // Target already holds target address.
    // Call and allocate arguments slots.
    jalr(target);
    // Use the branch delay slot to allocated argument slots.
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
    addiu(sp, sp, StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::CallBuiltin(Handle<Code> code, RelocInfo::Mode rmode) {
    ASSERT(RelocInfo::IsCodeTarget(rmode));
    // Jump but do not protect the branch delay slot.
    Call(false, code, rmode);
    // Use the branch delay slot to allocated argument slots.
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
    addiu(sp, sp, StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::JumpToBuiltin(ExternalReference builtin_entry) {
    // Load builtin address.
    LoadExternalReference(t9, builtin_entry);
    lw(t9, MemOperand(t9));  // Deref address.
    addiu(t9, t9, Code::kHeaderSize - kHeapObjectTag);
    // Call and allocate arguments slots.
    jr(t9);
    // Use the branch delay slot to allocated argument slots.
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::JumpToBuiltin(Register target) {
    // t9 already holds target address.
    // Call and allocate arguments slots.
    jr(t9);
    // Use the branch delay slot to allocated argument slots.
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  }


  void MacroAssembler::JumpToBuiltin(Handle<Code> code, RelocInfo::Mode rmode) {
    ASSERT(RelocInfo::IsCodeTarget(rmode));
    // Jump but do not protect the branch delay slot.
    Jump(false, code, rmode);
    // Use the branch delay slot to allocated argument slots.
    addiu(sp, sp, -StandardFrameConstants::kRArgsSlotsSize);
  }


// -----------------------------------------------------------------------------
// Runtime calls

void MacroAssembler::CallStub(CodeStub* stub, Condition cond,
                              Register r1, const Operand& r2) {
  ASSERT(allow_stub_calls());  // Stub calls are not allowed in some stubs.
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
  // All parameters are on the stack. v0 has the return value after call.

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
  LoadExternalReference(a1, ExternalReference(f));
  CEntryStub stub(1);
  CallStub(&stub);
}


void MacroAssembler::CallRuntime(Runtime::FunctionId fid, int num_arguments) {
  CallRuntime(Runtime::FunctionForId(fid), num_arguments);
}


void MacroAssembler::TailCallExternalReference(const ExternalReference& ext,
                                               int num_arguments,
                                               int result_size) {
  // TODO(1236192): Most runtime routines don't need the number of
  // arguments passed in because it is constant. At some point we
  // should remove this need and make the runtime routine entry code
  // smarter.
  li(a0, Operand(num_arguments));
  JumpToExternalReference(ext);
}


void MacroAssembler::TailCallRuntime(Runtime::FunctionId fid,
                                     int num_arguments,
                                     int result_size) {
  TailCallExternalReference(ExternalReference(fid), num_arguments, result_size);
}


void MacroAssembler::JumpToExternalReference(const ExternalReference& builtin) {
  li(a1, Operand(builtin));
  CEntryStub stub(1);
  Jump(stub.GetCode(), RelocInfo::CODE_TARGET);
}


void MacroAssembler::InvokeBuiltin(Builtins::JavaScript id,
                                   InvokeJSFlags flags) {
  GetBuiltinEntry(a2, id);
  if (flags == CALL_JS) {
    Call(a2);
  } else {
    ASSERT(flags == JUMP_JS);
    Jump(a2);
  }
}


void MacroAssembler::GetBuiltinEntry(Register target, Builtins::JavaScript id) {
  // Load the JavaScript builtin function from the builtins object.
  lw(a1, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  lw(a1, FieldMemOperand(a1, GlobalObject::kBuiltinsOffset));
  int builtins_offset =
      JSBuiltinsObject::kJSBuiltinsOffset + (id * kPointerSize);
  lw(a1, FieldMemOperand(a1, builtins_offset));
  // Load the code entry point from the function into the target register.
  lw(target, FieldMemOperand(a1, JSFunction::kSharedFunctionInfoOffset));
  lw(target, FieldMemOperand(target, SharedFunctionInfo::kCodeOffset));
  Add(target, target, Operand(Code::kHeaderSize - kHeapObjectTag));
}


void MacroAssembler::SetCounter(StatsCounter* counter, int value,
                                Register scratch1, Register scratch2) {
  UNIMPLEMENTED_MIPS();
}


void MacroAssembler::IncrementCounter(StatsCounter* counter, int value,
                                      Register scratch1, Register scratch2) {
  ASSERT(value > 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    li(scratch2, Operand(ExternalReference(counter)));
    lw(scratch1, MemOperand(scratch2));
    Add(scratch1, scratch1, Operand(value));
    sw(scratch1, MemOperand(scratch2));
  }
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


void MacroAssembler::EnterExitFrame(ExitFrame::Mode mode,
                                    Register hold_argc,
                                    Register hold_argv,
                                    Register hold_function) {
  // Compute the argv pointer and keep it in a callee-saved register.
  // a0 is argc.
  sll(t0, a0, kPointerSizeLog2);
  add(hold_argv, sp, t0);
  addi(hold_argv, hold_argv, -kPointerSize);

  // Compute callee's stack pointer before making changes and save it as
  // t1 register so that it is restored as sp register on exit, thereby
  // popping the args.
  // t1 = sp + kPointerSize * #args
  add(t1, sp, t0);

  // Align the stack at this point.
  AlignStack(0);

  // Save registers.
  addiu(sp, sp, -12);
  sw(t1, MemOperand(sp, 8));
  sw(ra, MemOperand(sp, 4));
  sw(fp, MemOperand(sp, 0));
  mov(fp, sp);  // Setup new frame pointer.

  // Push debug marker.
  if (mode == ExitFrame::MODE_DEBUG) {
    Push(zero_reg);
  } else {
    li(t0, Operand(CodeObject()));
    Push(t0);
  }

  // Save the frame pointer and the context in top.
  LoadExternalReference(t0, ExternalReference(Top::k_c_entry_fp_address));
  sw(fp, MemOperand(t0));
  LoadExternalReference(t0, ExternalReference(Top::k_context_address));
  sw(cp, MemOperand(t0));

  // Setup argc and the builtin function in callee-saved registers.
  mov(hold_argc, a0);
  mov(hold_function, a1);
}


void MacroAssembler::LeaveExitFrame(ExitFrame::Mode mode) {
  // Clear top frame.
  LoadExternalReference(t0, ExternalReference(Top::k_c_entry_fp_address));
  sw(zero_reg, MemOperand(t0));

  // Restore current context from top and clear it in debug mode.
  LoadExternalReference(t0, ExternalReference(Top::k_context_address));
  lw(cp, MemOperand(t0));
#ifdef DEBUG
  sw(a3, MemOperand(t0));
#endif

  // Pop the arguments, restore registers, and return.
  mov(sp, fp);  // Respect ABI stack constraint.
  lw(fp, MemOperand(sp, 0));
  lw(ra, MemOperand(sp, 4));
  lw(sp, MemOperand(sp, 8));
  jr(ra);
  nop();  // Branch delay slot nop.
}


void MacroAssembler::AlignStack(int offset) {
  // On MIPS an offset of 0 aligns to 0 modulo 8 bytes,
  //     and an offset of 1 aligns to 4 modulo 8 bytes.
#if defined(V8_HOST_ARCH_MIPS)
  // Running on the real platform. Use the alignment as mandated by the local
  // environment.
  // Note: This will break if we ever start generating snapshots on one MIPS
  // platform for another MIPS platform with a different alignment.
  int activation_frame_alignment = OS::ActivationFrameAlignment();
#else  // defined(V8_HOST_ARCH_MIPS)
  // If we are using the simulator then we should always align to the expected
  // alignment. As the simulator is used to generate snapshots we do not know
  // if the target platform will need alignment, so we will always align at
  // this point here.
  int activation_frame_alignment = 2 * kPointerSize;
#endif  // defined(V8_HOST_ARCH_MIPS)
  if (activation_frame_alignment != kPointerSize) {
    // This code needs to be made more general if this assert doesn't hold.
    ASSERT(activation_frame_alignment == 2 * kPointerSize);
    if (offset == 0) {
      andi(t0, sp, activation_frame_alignment - 1);
      Push(zero_reg, eq, t0, zero_reg);
    } else {
      andi(t0, sp, activation_frame_alignment - 1);
      addiu(t0, t0, -4);
      Push(zero_reg, eq, t0, zero_reg);
    }
  }
}

} }  // namespace v8::internal

