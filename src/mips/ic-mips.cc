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
#include "ic-inl.h"
#include "runtime.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {


// ----------------------------------------------------------------------------
// Static IC stub generators.
//

#define __ ACCESS_MASM(masm)


void LoadIC::GenerateArrayLength(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
}


void LoadIC::GenerateStringLength(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
}


void LoadIC::GenerateFunctionPrototype(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
}


// Defined in ic.cc.
Object* CallIC_Miss(Arguments args);

void CallIC::GenerateMegamorphic(MacroAssembler* masm, int argc) {
  UNIMPLEMENTED_MIPS();
}


void CallIC::GenerateNormal(MacroAssembler* masm, int argc) {
  UNIMPLEMENTED_MIPS();
}

void CallIC::GenerateMiss(MacroAssembler* masm, int argc) {
    // Registers:
    // a2: name
    // ra: return address

  // Get the receiver of the function from the stack.
  __ lw(a3, MemOperand(sp, argc*kPointerSize));

  __ EnterInternalFrame();

  // Push the receiver and the name of the function.
  __ MultiPush(a2.bit() | a3.bit());

  // Call the entry.
  __ li(a0, Operand(2));
  __ li(a1, Operand(ExternalReference(IC_Utility(kCallIC_Miss))));

  CEntryStub stub(1);
  __ CallStub(&stub);

  // Move result to r1 and leave the internal frame.
  __ mov(a1, v0);
  __ LeaveInternalFrame();

  // Check if the receiver is a global object of some sort.
  Label invoke, global;
  __ lw(a2, MemOperand(sp, argc * kPointerSize));
  __ andi(t0, a2, kSmiTagMask);
  __ Branch(eq, &invoke, t0, Operand(zero_reg));
  __ GetObjectType(a2, a3, a3);
  __ Branch(eq, &global, a3, Operand(JS_GLOBAL_OBJECT_TYPE));
  __ Branch(ne, &invoke, a3, Operand(JS_BUILTINS_OBJECT_TYPE));

  // Patch the receiver on the stack.
  __ bind(&global);
  __ lw(a2, FieldMemOperand(a2, GlobalObject::kGlobalReceiverOffset));
  __ sw(a2, MemOperand(sp, argc * kPointerSize));

  // Invoke the function.
  ParameterCount actual(argc);
  __ bind(&invoke);
  __ InvokeFunction(a1, actual, JUMP_FUNCTION);
}

// Defined in ic.cc.
Object* LoadIC_Miss(Arguments args);

void LoadIC::GenerateMegamorphic(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x125);
}


void LoadIC::GenerateNormal(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x131);
}


void LoadIC::GenerateMiss(MacroAssembler* masm) {
  // a2    : name
  // ra    : return address
  // [sp]  : receiver

  __ lw(a3, MemOperand(sp));
  __ MultiPush(a2.bit() | a3.bit());

  // Perform tail call to the entry.
  ExternalReference ref = ExternalReference(IC_Utility(kLoadIC_Miss));
  __ TailCallExternalReference(ref, 2, 1);
}


void LoadIC::ClearInlinedVersion(Address address) {}
bool LoadIC::PatchInlinedLoad(Address address, Object* map, int offset) {
  return false;
}

void KeyedLoadIC::ClearInlinedVersion(Address address) {}
bool KeyedLoadIC::PatchInlinedLoad(Address address, Object* map) {
  return false;
}

void KeyedStoreIC::ClearInlinedVersion(Address address) {}
void KeyedStoreIC::RestoreInlinedVersion(Address address) {}
bool KeyedStoreIC::PatchInlinedStore(Address address, Object* map) {
  return false;
}


Object* KeyedLoadIC_Miss(Arguments args);


void KeyedLoadIC::GenerateMiss(MacroAssembler* masm) {
  // ra     : return address
  // sp[0]  : key
  // sp[4]  : receiver

  __ lw(a2, MemOperand(sp, 0));
  __ lw(a3, MemOperand(sp, 4));
  __ MultiPush(a2.bit() | a3.bit());

  ExternalReference ref = ExternalReference(IC_Utility(kKeyedLoadIC_Miss));
  __ TailCallExternalReference(ref, 2, 1);
}


void KeyedLoadIC::GenerateGeneric(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
}


void KeyedLoadIC::GenerateString(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
}


void KeyedLoadIC::GenerateExternalArray(MacroAssembler* masm,
                                        ExternalArrayType array_type) {
  UNIMPLEMENTED_MIPS();
}


void KeyedStoreIC::GenerateGeneric(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x191);
}


void KeyedStoreIC::GenerateExternalArray(MacroAssembler* masm,
                                         ExternalArrayType array_type) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x197);
}


void KeyedLoadIC::GenerateIndexedInterceptor(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x204);
}


void KeyedStoreIC::GenerateMiss(MacroAssembler* masm) {
  // a0     : value
  // ra     : return address
  // sp[0]  : key
  // sp[1]  : receiver

  __ lw(a3, MemOperand(sp, 1 * kPointerSize));
  __ lw(a2, MemOperand(sp, 0 * kPointerSize));
  __ MultiPush(a0.bit() | a2.bit() | a3.bit());

  ExternalReference ref = ExternalReference(IC_Utility(kKeyedStoreIC_Miss));
  __ TailCallExternalReference(ref, 3, 1);
}


void StoreIC::GenerateMegamorphic(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x216);
}


void StoreIC::GenerateMiss(MacroAssembler* masm) {
  // a0    : value
  // a1    : receiver
  // a2    : name
  // ra    : return address

  __ addiu(sp, sp, -3 * kPointerSize);
  __ sw(a1, MemOperand(sp, 2 * kPointerSize));
  __ sw(a2, MemOperand(sp, 1 * kPointerSize));
  __ sw(a0, MemOperand(sp, 0 * kPointerSize));

  // Perform tail call to the entry.
  ExternalReference ref = ExternalReference(IC_Utility(kStoreIC_Miss));
  __ TailCallExternalReference(ref, 3, 1);
}


void StoreIC::GenerateArrayLength(MacroAssembler* masm) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x224);
}

#undef __

} }  // namespace v8::internal

