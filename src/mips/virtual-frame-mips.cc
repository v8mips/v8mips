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

#include "codegen-inl.h"
#include "register-allocator-inl.h"
#include "scopes.h"
#include "virtual-frame-inl.h"

namespace v8 {
namespace internal {

// -------------------------------------------------------------------------
// VirtualFrame implementation.

#define __ ACCESS_MASM(masm())

void VirtualFrame::SyncElementBelowStackPointer(int index) {
  UNREACHABLE();
}


void VirtualFrame::SyncElementByPushing(int index) {
  UNREACHABLE();
}


void VirtualFrame::MergeTo(VirtualFrame* expected) {
  // MIPS frames are currently always in memory.
  ASSERT(Equals(expected));
}


void VirtualFrame::Enter() {
  Comment cmnt(masm(), "[ Enter JS frame");

#ifdef DEBUG
  // Verify that r1 contains a JS function.  The following code relies
  // on r2 being available for use.
  if (FLAG_debug_code) {
    Label map_check, done;
    __ BranchOnNotSmi(a1, &map_check, t1);
    __ stop("VirtualFrame::Enter - a1 is not a function (smi check).");
    __ bind(&map_check);
    __ GetObjectType(a1, a2, a2);
    __ Branch(&done, eq, a2, Operand(JS_FUNCTION_TYPE));
    __ stop("VirtualFrame::Enter - a1 is not a function (map check).");
    __ bind(&done);
  }
#endif  // DEBUG

  // We are about to push four values to the frame.
  Adjust(4);
  __ MultiPush(ra.bit() | fp.bit() | cp.bit() | a1.bit());
  // Adjust FP to point to saved FP.
  __ addiu(fp, sp, 2 * kPointerSize);
}


void VirtualFrame::Exit() {
  Comment cmnt(masm(), "[ Exit JS frame");
  // Record the location of the JS exit code for patching when setting
  // break point.
  __ RecordJSReturn();

  // Drop the execution stack down to the frame pointer and restore the caller
  // frame pointer and return address.
  __ mov(sp, fp);
  __ lw(fp, MemOperand(sp, 0));
  __ lw(ra, MemOperand(sp, 4));
  __ addiu(sp, sp, 8);
}


void VirtualFrame::AllocateStackSlots() {
  int count = local_count();
  __ LoadRoot(t2, Heap::kStackLimitRootIndex);
  if (count > 0) {
    Comment cmnt(masm(), "[ Allocate space for locals");
    Adjust(count);
    // Initialize stack slots with 'undefined' value.
    __ LoadRoot(t0, Heap::kUndefinedValueRootIndex);
    __ addiu(sp, sp, -count * kPointerSize);
    if (count < kLocalVarBound) {
      // For less locals the unrolled loop is more compact.
      for (int i = 0; i < count; i++) {
        __ sw(t0, MemOperand(sp, (count-i-1)*kPointerSize));
      }
    } else {
      // For more locals a loop in generated code is more compact.
      Label alloc_locals_loop;
      __ li(a1, Operand(count));
      __ mov(a2, sp);
      __ bind(&alloc_locals_loop);
      __ sw(t0, MemOperand(a2, 0));
      __ Subu(a1, a1, Operand(1));
      __ Branch(false, &alloc_locals_loop, gt, a1, Operand(zero_reg));
      __ Addu(a2, a2, Operand(kPointerSize)); // Use branch-delay slot.
    }
  }
  // Call the stub if lower.
  Label stack_ok;
  __ Branch(&stack_ok, Uless, t2, Operand(sp));
  StackCheckStub stub;
  __ Push(ra);
  __ Call(Operand(reinterpret_cast<intptr_t>(stub.GetCode().location()),
          RelocInfo::CODE_TARGET));
  __ Pop(ra);
  __ bind(&stack_ok);
}


void VirtualFrame::SaveContextRegister() {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::RestoreContextRegister() {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::PushReceiverSlotAddress() {
  UNIMPLEMENTED_MIPS();
}


int VirtualFrame::InvalidateFrameSlotAt(int index) {
  return kIllegalIndex;
}


void VirtualFrame::TakeFrameSlotAt(int index) {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::StoreToFrameSlotAt(int index) {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::PushTryHandler(HandlerType type) {
  // Grow the expression stack by handler size less one (the return
  // address in lr is already counted by a call instruction).
  Adjust(kHandlerSize - 1);
  __ PushTryHandler(IN_JAVASCRIPT, type);
}


void VirtualFrame::RawCallStub(CodeStub* stub) {
  ASSERT(cgen()->HasValidEntryRegisters());
  __ CallStub(stub);
}


void VirtualFrame::CallStub(CodeStub* stub, Result* arg) {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::CallStub(CodeStub* stub, Result* arg0, Result* arg1) {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::CallRuntime(Runtime::Function* f, int arg_count) {
  Forget(arg_count);
  ASSERT(cgen()->HasValidEntryRegisters());
  __ CallRuntime(f, arg_count);
}


void VirtualFrame::CallRuntime(Runtime::FunctionId id, int arg_count) {
  Forget(arg_count);
  ASSERT(cgen()->HasValidEntryRegisters());
  __ CallRuntime(id, arg_count);
}


#ifdef ENABLE_DEBUGGER_SUPPORT
void VirtualFrame::DebugBreak() {
  ASSERT(cgen()->HasValidEntryRegisters());
  __ DebugBreak();
}
#endif


void VirtualFrame::CallAlignedRuntime(Runtime::Function* f, int arg_count) {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::CallAlignedRuntime(Runtime::FunctionId id, int arg_count) {
  UNIMPLEMENTED_MIPS();
}


void VirtualFrame::InvokeBuiltin(Builtins::JavaScript id,
                                 InvokeJSFlags flags,
                                 int arg_count) {
  Forget(arg_count);
  __ InvokeBuiltin(id, flags);
}


void VirtualFrame::CallCodeObject(Handle<Code> code,
                                  RelocInfo::Mode rmode,
                                  int dropped_args) {
  switch (code->kind()) {
    case Code::CALL_IC:
      Forget(dropped_args);
      ASSERT(cgen()->HasValidEntryRegisters());
      __ Call(code, rmode);
      break;

    case Code::FUNCTION:
      UNIMPLEMENTED_MIPS();
      __ break_(__LINE__);
      break;

    case Code::KEYED_LOAD_IC:
    case Code::LOAD_IC:
    case Code::KEYED_STORE_IC:
    case Code::STORE_IC:
      ASSERT(dropped_args == 0);
      Forget(dropped_args);
      ASSERT(cgen()->HasValidEntryRegisters());
      __ Call(code, rmode);
      break;

    case Code::BUILTIN:
      // The only builtin called through this function is JSConstructCall.
      ASSERT(*code == Builtins::builtin(Builtins::JSConstructCall));
      Forget(dropped_args);
      ASSERT(cgen()->HasValidEntryRegisters());
      // This is a builtin and it expects argument slots.
      // Don't protect the branch delay slot and use it to allocate args slots.
      __ Call(false, code, rmode);
      __ addiu(sp, sp, -StandardFrameConstants::kBArgsSlotsSize);
      __ addiu(sp, sp, StandardFrameConstants::kBArgsSlotsSize);
      break;

    default:
      UNREACHABLE();
      break;
  }
}


void VirtualFrame::Drop(int count) {
  ASSERT(count >= 0);
  ASSERT(height() >= count);
  int num_virtual_elements = (element_count() - 1) - stack_pointer_;

  // Emit code to lower the stack pointer if necessary.
  if (num_virtual_elements < count) {
    int num_dropped = count - num_virtual_elements;
    stack_pointer_ -= num_dropped;
    __ addiu(sp, sp, num_dropped * kPointerSize);
  }

  // Discard elements from the virtual frame and free any registers.
  element_count_ -= count;
}


void VirtualFrame::DropFromVFrameOnly(int count) {
  UNIMPLEMENTED_MIPS();
}


Result VirtualFrame::Pop() {
  UNIMPLEMENTED_MIPS();
  Result res = Result();
  return res;    // UNIMPLEMENTED RETURN
}


void VirtualFrame::EmitPop(Register reg) {
  ASSERT(stack_pointer_ == element_count() - 1);
  stack_pointer_--;
  element_count_--;
  __ Pop(reg);
}


void VirtualFrame::EmitMultiPop(RegList regs) {
  ASSERT(stack_pointer_ == element_count() - 1);
  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      stack_pointer_--;
      element_count_--;
    }
  }
  __ MultiPop(regs);
}


void VirtualFrame::EmitPush(Register reg) {
  ASSERT(stack_pointer_ == element_count() - 1);
  element_count_++
  stack_pointer_++;
  __ Push(reg);
}


void VirtualFrame::EmitMultiPush(RegList regs) {
  ASSERT(stack_pointer_ == element_count() - 1);
  for (int16_t i = kNumRegisters; i > 0; i--) {
    if ((regs & (1 << i)) != 0) {
      element_count_++
      stack_pointer_++;
    }
  }
  __ MultiPush(regs);
}


void VirtualFrame::EmitMultiPushReversed(RegList regs) {
  ASSERT(stack_pointer_ == element_count() - 1);
  for (int16_t i = 0; i< RegisterAllocatorConstants::kNumRegisters; i++) {
    if ((regs & (1<<i)) != 0) {
      elements_.Add(FrameElement::MemoryElement(TypeInfo::Unknown()));
      stack_pointer_++;
    }
  }
  __ MultiPushReversed(regs);
}


void VirtualFrame::EmitArgumentSlots(RegList reglist) {
  UNIMPLEMENTED_MIPS();
}

#undef __

} }  // namespace v8::internal

