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

#if defined(V8_TARGET_ARCH_MIPS)

#include "codegen-inl.h"
#include "debug.h"

namespace v8 {
namespace internal {

#ifdef ENABLE_DEBUGGER_SUPPORT

bool BreakLocationIterator::IsDebugBreakAtReturn() {
  return Debug::IsDebugBreakAtReturn(rinfo());
}


void BreakLocationIterator::SetDebugBreakAtReturn() {
  // Mips return sequence:
  // mov sp, fp
  // lw fp, sp(0)
  // lw ra, sp(4)
  // addiu sp, sp, 8
  // addiu sp, sp, N
  // jr ra
  // nop (in branch delay slot)

  // Make sure this constant matches the number if instrucntions we emit.
  ASSERT(Assembler::kJSReturnSequenceInstructions == 7);
  CodePatcher patcher(rinfo()->pc(), Assembler::kJSReturnSequenceInstructions);
  // li and Call pseudo-instructions emit two instructions each.
  patcher.masm()->li(v8::internal::t9,
      Operand(reinterpret_cast<int32_t>(Debug::debug_break_return()->entry())));
  patcher.masm()->Call(v8::internal::t9);
  patcher.masm()->nop();
  patcher.masm()->nop();
  patcher.masm()->nop();

  // TODO(mips): Open issue about using breakpoint instruction instead of nops.
  // patcher.masm()->bkpt(0);
}


// Restore the JS frame exit code.
void BreakLocationIterator::ClearDebugBreakAtReturn() {
  rinfo()->PatchCode(original_rinfo()->pc(),
                     Assembler::kJSReturnSequenceInstructions);
}


// A debug break in the exit code is identified by the JS frame exit code
// having been patched with li/call psuedo-instrunction (liu/ori/jalr)
bool Debug::IsDebugBreakAtReturn(RelocInfo* rinfo) {
  ASSERT(RelocInfo::IsJSReturn(rinfo->rmode()));
  return rinfo->IsPatchedReturnSequence();
}


bool BreakLocationIterator::IsDebugBreakAtSlot() {
  ASSERT(IsDebugBreakSlot());
  // Check whether the debug break slot instructions have been patched.
  return rinfo()->IsPatchedDebugBreakSlotSequence();
}


void BreakLocationIterator::SetDebugBreakAtSlot() {
  ASSERT(IsDebugBreakSlot());
  // Patch the code changing the debug break slot code from
  //   nop(2) - nop(2) is sll(zero_reg, zero_reg, 2)
  //   nop(2)
  //   nop(2)
  //   nop(2)
  // to a call to the debug break slot code.
  //   li t9, address   (lui t9 / ori t9 instruction pair)
  //   call t9          (jalr t9 / nop instruction pair)
  CodePatcher patcher(rinfo()->pc(), Assembler::kDebugBreakSlotInstructions);
  patcher.masm()->li(v8::internal::t9,
      Operand(reinterpret_cast<int32_t>(Debug::debug_break_return()->entry())));
  patcher.masm()->Call(v8::internal::t9);
}


void BreakLocationIterator::ClearDebugBreakAtSlot() {
  ASSERT(IsDebugBreakSlot());
  rinfo()->PatchCode(original_rinfo()->pc(),
                     Assembler::kDebugBreakSlotInstructions);
}


#define __ ACCESS_MASM(masm)



static void Generate_DebugBreakCallHelper(MacroAssembler* masm,
                                          RegList pointer_regs) {
  // Save the content of all general purpose registers in memory. This copy in
  // memory is later pushed onto the JS expression stack for the fake JS frame
  // generated and also to the C frame generated on top of that. In the JS
  // frame ONLY the registers containing pointers will be pushed on the
  // expression stack. This causes the GC to update these  pointers so that
  // they will have the correct value when returning from the debugger.
  __ SaveRegistersToMemory(kJSCallerSaved);

  __ EnterInternalFrame();

  // Store the registers containing object pointers on the expression stack to
  // make sure that these are correctly updated during GC.
  // Use sp as base to push.
  __ CopyRegistersFromMemoryToStack(sp, pointer_regs);

#ifdef DEBUG
  __ RecordComment("// Calling from debug break to runtime - come in - over");
#endif
  __ mov(a0, zero_reg);  // no arguments
  __ li(a1, Operand(ExternalReference::debug_break()));

  CEntryStub ceb(1, ExitFrame::MODE_DEBUG);
  __ CallStub(&ceb);

  // Restore the register values containing object pointers from the expression
  // stack in the reverse order as they where pushed.
  // Use sp as base to pop.
  __ CopyRegistersFromStackToMemory(sp, a3, pointer_regs);

  __ LeaveInternalFrame();

  // Finally restore all registers.
  __ RestoreRegistersFromMemory(kJSCallerSaved);

  // Now that the break point has been handled, resume normal execution by
  // jumping to the target address intended by the caller and that was
  // overwritten by the address of DebugBreakXXX.
  __ li(t9, Operand(ExternalReference(Debug_Address::AfterBreakTarget())));
  __ lw(t9, MemOperand(t9));
  __ Jump(t9);
}


void Debug::GenerateLoadICDebugBreak(MacroAssembler* masm) {
  // Calling convention for IC load (from ic-mips.cc).
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  //  -- a0    : receiver
  //  -- [sp]  : receiver
  // -----------------------------------
  // Registers a0 and a2 contain objects that need to be pushed on the
  // expression stack of the fake JS frame.
  Generate_DebugBreakCallHelper(masm, a0.bit() | a2.bit());
}


void Debug::GenerateStoreICDebugBreak(MacroAssembler* masm) {
  // Calling convention for IC store (from ic-mips.cc).
  // ----------- S t a t e -------------
  //  -- a0    : value
  //  -- a1    : receiver
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------
  // Registers a0, a1, and a2 contain objects that need to be pushed on the
  // expression stack of the fake JS frame.
  Generate_DebugBreakCallHelper(masm, a0.bit() | a1.bit() | a2.bit());
}


void Debug::GenerateKeyedLoadICDebugBreak(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- sp[0]  : key
  //  -- sp[4]  : receiver
  Generate_DebugBreakCallHelper(masm, a0.bit());
}


void Debug::GenerateKeyedStoreICDebugBreak(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- a1     : key
  //  -- a2     : receiver
  //  -- ra     : return address
  Generate_DebugBreakCallHelper(masm, a0.bit() | a1.bit() | a2.bit());
}


void Debug::GenerateCallICDebugBreak(MacroAssembler* masm) {
  // Calling convention for IC call (from ic-mips.cc)
  // ----------- S t a t e -------------
  //  -- a0: number of arguments
  //  -- a1: receiver
  //  -- ra: return address
  // -----------------------------------
  // Register a1 contains an object that needs to be pushed on the expression
  // stack of the fake JS frame. a0 is the actual number of arguments not
  // encoded as a smi, therefore it cannot be on the expression stack of the
  // fake JS frame as it can easily be an invalid pointer (e.g. 1). a0 will be
  // pushed on the stack of the C frame and restored from there.
  Generate_DebugBreakCallHelper(masm, a1.bit());
}


void Debug::GenerateConstructCallDebugBreak(MacroAssembler* masm) {
  // In places other than IC call sites it is expected that a0 is TOS which
  // is an object - this is not generally the case so this should be used with
  // care.
  Generate_DebugBreakCallHelper(masm, a0.bit());
}


void Debug::GenerateReturnDebugBreak(MacroAssembler* masm) {
  // In places other than IC call sites it is expected that a0 is TOS which
  // is an object - this is not generally the case so this should be used with
  // care.
  Generate_DebugBreakCallHelper(masm, a0.bit());
}


void Debug::GenerateStubNoRegistersDebugBreak(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  No registers used on entry.
  // -----------------------------------
  Generate_DebugBreakCallHelper(masm, 0);
}


void Debug::GenerateSlot(MacroAssembler* masm) {
  // Generate enough nop's to make space for a call instruction. Avoid emitting
  // the trampoline pool in the debug break slot code.
  Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm);
  Label check_codesize;
  __ bind(&check_codesize);
  __ RecordDebugBreakSlot();
  for (int i = 0; i < Assembler::kDebugBreakSlotInstructions; i++) {
    __ nop(2);
  }
  ASSERT_EQ(Assembler::kDebugBreakSlotInstructions,
            masm->InstructionsGeneratedSince(&check_codesize));
}


void Debug::GenerateSlotDebugBreak(MacroAssembler* masm) {
  // In the places where a debug break slot is inserted no registers can contain
  // object pointers.
  Generate_DebugBreakCallHelper(masm, 0);
}


void Debug::GeneratePlainReturnLiveEdit(MacroAssembler* masm) {
  masm->Abort("LiveEdit frame dropping is not supported on mips");
}

void Debug::GenerateFrameDropperLiveEdit(MacroAssembler* masm) {
  masm->Abort("LiveEdit frame dropping is not supported on mips");
}


const bool Debug::kFrameDropperSupported = false;

#undef __


#endif  // ENABLE_DEBUGGER_SUPPORT

} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_MIPS
