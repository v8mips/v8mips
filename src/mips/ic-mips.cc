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


// Helper function used from LoadIC/CallIC GenerateNormal.
static void GenerateDictionaryLoad(MacroAssembler* masm,
                                   Label* miss,
                                   Register reg0,
                                   Register reg1) {
  // Register use:
  //
  // reg0 - used to hold the property dictionary.
  //
  // reg1 - initially the receiver
  //    - used for the index into the property dictionary
  //    - holds the result on exit.
  //
  // a3 - used as temporary and to hold the capacity of the property
  //      dictionary.
  //
  // a2 - holds the name of the property and is unchanged.
  //
  // t0 - scratch

  Label done;

  // Check for the absence of an interceptor.
  // Load the map into reg0.
  __ lw(reg0, FieldMemOperand(reg1, JSObject::kMapOffset));
  // Test the has_named_interceptor bit in the map.
  __ lw(a3, FieldMemOperand(reg0, Map::kInstanceAttributesOffset));
  __ And(a3, a3, Operand(1 << (Map::kHasNamedInterceptor + (3 * 8))));
  // Jump to miss if the interceptor bit is set.
  __ Branch(miss, ne, t0, Operand(zero_reg));

  // Bail out if we have a JS global proxy object.
  __ lbu(a3, FieldMemOperand(reg0, Map::kInstanceTypeOffset));
  __ Branch(miss, eq, a3, Operand(JS_GLOBAL_PROXY_TYPE));

  // Possible work-around for http://crbug.com/16276.
  // See also: http://codereview.chromium.org/155418.
  __ Branch(miss, eq, a3, Operand(JS_GLOBAL_OBJECT_TYPE));
  __ Branch(miss, eq, a3, Operand(JS_BUILTINS_OBJECT_TYPE));

  // Check that the properties array is a dictionary.
  __ lw(reg0, FieldMemOperand(reg1, JSObject::kPropertiesOffset));
  __ lw(a3, FieldMemOperand(reg0, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::kHashTableMapRootIndex);
  __ Branch(miss, ne, a3, Operand(t0));

  // Compute the capacity mask.
  const int kCapacityOffset = StringDictionary::kHeaderSize +
      StringDictionary::kCapacityIndex * kPointerSize;
  __ lw(a3, FieldMemOperand(reg0, kCapacityOffset));
  __ sra(a3, a3, kSmiTagSize);  // Convert smi to int.
  __ Subu(a3, a3, Operand(1));

  const int kElementsStartOffset = StringDictionary::kHeaderSize +
      StringDictionary::kElementsStartIndex * kPointerSize;

  // Generate an unrolled loop that performs a few probes before
  // giving up. Measurements done on Gmail indicate that 2 probes
  // cover ~93% of loads from dictionaries.
  static const int kProbes = 4;
  for (int i = 0; i < kProbes; i++) {
    // Compute the masked index: (hash + i + i * i) & mask.
    __ lw(reg1, FieldMemOperand(a2, String::kHashFieldOffset));
    if (i > 0) {
      // Add the probe offset (i + i * i) left shifted to avoid right shifting
      // the hash in a separate instruction. The value hash + i + i * i is right
      // shifted in the following and instruction.
      ASSERT(StringDictionary::GetProbeOffset(i) <
             1 << (32 - String::kHashFieldOffset));
      __ Addu(reg1, reg1, Operand(
          StringDictionary::GetProbeOffset(i) << String::kHashShift));
    }

    __ srl(reg1, reg1, String::kHashShift);
    __ And(reg1, a3, reg1);

    // Scale the index by multiplying by the element size.
    ASSERT(StringDictionary::kEntrySize == 3);
    __ sll(at, reg1, 1);  // at = reg1 * 2.
    __ addu(reg1, reg1, at);  // reg1 = reg1 * 3.

    // Check if the key is identical to the name.
    __ sll(at, reg1, 2);
    __ addu(reg1, reg0, at);
    __ lw(t0, FieldMemOperand(reg1, kElementsStartOffset));
    if (i != kProbes - 1) {
      __ Branch(&done, eq, a2, Operand(t0));
    } else {
      __ Branch(miss, ne, a2, Operand(t0));
    }
  }

  // Check that the value is a normal property.
  __ bind(&done);  // reg1 == reg0 + 4*index
  __ lw(a3, FieldMemOperand(reg1, kElementsStartOffset + 2 * kPointerSize));
  __ And(a3, a3, Operand(PropertyDetails::TypeField::mask() << kSmiTagSize));
  __ Branch(miss, ne, a3, Operand(zero_reg));

  // Get the value at the masked, scaled index and return.
  __ lw(reg1, FieldMemOperand(reg1, kElementsStartOffset + 1 * kPointerSize));
}


void LoadIC::GenerateArrayLength(MacroAssembler* masm) {
  // a2    : name
  // ra    : return address
  // [sp]  : receiver

  Label miss;

  __ lw(a0, MemOperand(sp, 0));

  StubCompiler::GenerateLoadArrayLength(masm, a0, a3, &miss);
  __ bind(&miss);
  StubCompiler::GenerateLoadMiss(masm, Code::LOAD_IC);
}


void LoadIC::GenerateStringLength(MacroAssembler* masm) {
  // a2    : name
  // lr    : return address
  // [sp]  : receiver
  Label miss;

  __ lw(a0, MemOperand(sp, 0));

  StubCompiler::GenerateLoadStringLength(masm, a0, a1, a3, &miss);
  // Cache miss: Jump to runtime.
  __ bind(&miss);
  StubCompiler::GenerateLoadMiss(masm, Code::LOAD_IC);
}


void LoadIC::GenerateFunctionPrototype(MacroAssembler* masm) {
  // r2    : name
  // lr    : return address
  // [sp]  : receiver

  Label miss;

  // Load receiver.
  __ lw(a0, MemOperand(sp, 0));

  StubCompiler::GenerateLoadFunctionPrototype(masm, a0, a1, a3, &miss);
  __ bind(&miss);
  StubCompiler::GenerateLoadMiss(masm, Code::LOAD_IC);
}


// Defined in ic.cc.
Object* CallIC_Miss(Arguments args);

void CallIC::GenerateMegamorphic(MacroAssembler* masm, int argc) {
  // r2    : name
  // lr: return address

  Label number, non_number, non_string, boolean, probe, miss;

  // Get the receiver of the function from the stack into r1.
  __ lw(a1, MemOperand(sp, argc * kPointerSize));

  // Probe the stub cache.
  Code::Flags flags =
      Code::ComputeFlags(Code::CALL_IC, NOT_IN_LOOP, MONOMORPHIC, NORMAL, argc);
  StubCache::GenerateProbe(masm, flags, a1, a2, a3, no_reg);

  // If the stub cache probing failed, the receiver might be a value.
  // For value objects, we use the map of the prototype objects for
  // the corresponding JSValue for the cache and that is what we need
  // to probe.
  //
  // Check for number.
  __ BranchOnSmi(a1, &number, t1);
  __ GetObjectType(a1, a3, a3);
  __ Branch(&non_number, ne, a3, Operand(HEAP_NUMBER_TYPE));
  __ bind(&number);
  StubCompiler::GenerateLoadGlobalFunctionPrototype(
      masm, Context::NUMBER_FUNCTION_INDEX, a1);
  __ Branch(&probe);

  // Check for string.
  __ bind(&non_number);
  __ Branch(&non_string, Ugreater_equal, a3, Operand(FIRST_NONSTRING_TYPE));
  StubCompiler::GenerateLoadGlobalFunctionPrototype(
      masm, Context::STRING_FUNCTION_INDEX, a1);
  __ Branch(&probe);


  // Check for boolean.
  __ bind(&non_string);
  __ LoadRoot(t0, Heap::kTrueValueRootIndex);
  __ Branch(&boolean, eq, a1, Operand(t0));
  __ LoadRoot(t1, Heap::kFalseValueRootIndex);
  __ Branch(&miss, ne, a1, Operand(t1));
  __ bind(&boolean);
  StubCompiler::GenerateLoadGlobalFunctionPrototype(
      masm, Context::BOOLEAN_FUNCTION_INDEX, a1);

  // Probe the stub cache for the value object.
  __ bind(&probe);
  StubCache::GenerateProbe(masm, flags, a1, a2, a3, no_reg);

  // Cache miss: Jump to runtime.
  __ bind(&miss);
  GenerateMiss(masm, argc);
}


void CallIC::GenerateNormal(MacroAssembler* masm, int argc) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
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

  // Move result to a1 and leave the internal frame.
  __ mov(a1, v0);
  __ LeaveInternalFrame();

  // Check if the receiver is a global object of some sort.
  Label invoke, global;
  __ lw(a2, MemOperand(sp, argc * kPointerSize));
  __ andi(t0, a2, kSmiTagMask);
  __ Branch(&invoke, eq, t0, Operand(zero_reg));
  __ GetObjectType(a2, a3, a3);
  __ Branch(&global, eq, a3, Operand(JS_GLOBAL_OBJECT_TYPE));
  __ Branch(&invoke, ne, a3, Operand(JS_BUILTINS_OBJECT_TYPE));

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
  // a2    : name
  // ra    : return address
  // [sp]  : receiver

  __ lw(a0, MemOperand(sp, 0));
  // Probe the stub cache.
  Code::Flags flags = Code::ComputeFlags(Code::LOAD_IC,
                                         NOT_IN_LOOP,
                                         MONOMORPHIC);
  StubCache::GenerateProbe(masm, flags, a0, a2, a3, no_reg);

  // Cache miss: Jump to runtime.
  GenerateMiss(masm);
}


void LoadIC::GenerateNormal(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- lr    : return address
  //  -- [sp]  : receiver
  // -----------------------------------
  Label miss, probe, global;

  __ lw(a0, MemOperand(sp, 0));
  // Check that the receiver isn't a smi.
  __ BranchOnSmi(a0, &miss);

  // Check that the receiver is a valid JS object.  Put the map in a3.
  __ GetObjectType(a0, a3, a1);
  __ Branch(&miss, lt, a1, Operand(FIRST_JS_OBJECT_TYPE));

  // If this assert fails, we have to check upper bound too.
  ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);

  // Check for access to global object (unlikely).
  __ Branch(&global, eq, a1, Operand(JS_GLOBAL_PROXY_TYPE));

  // Check for non-global object that requires access check.
  // Note that we trash map pointer (a3) here.
  __ lbu(a3, FieldMemOperand(a3, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&miss, ne, a3, Operand(zero_reg));

  __ bind(&probe);
  // This function uses name in a2, and trashes a3.
  GenerateDictionaryLoad(masm, &miss, a1, a0);
  __ Ret();

  // Global object access: Check access rights.
  __ bind(&global);
  __ CheckAccessGlobalProxy(a0, a1, &miss);
  __ Branch(&probe);

  // Cache miss: Restore receiver from stack and jump to runtime.
  __ bind(&miss);
  GenerateMiss(masm);
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


void KeyedLoadIC::GenerateRuntimeGetProperty(MacroAssembler* masm) {
  // ra     : return address
  // sp[0]  : key
  // sp[4]  : receiver

  __ lw(a2, MemOperand(sp, 0));
  __ lw(a3, MemOperand(sp, 4));
  __ MultiPush(a2.bit() | a3.bit());
  // Do a tail-call to runtime routine.

  __ TailCallRuntime(Runtime::kGetProperty, 2, 1);
}


void KeyedLoadIC::GenerateGeneric(MacroAssembler* masm) {
  // ra     : return address
  // sp[0]  : key
  // sp[4]  : receiver
  Label slow, fast, check_pixel_array;

  // Get the key and receiver object from the stack.
  __ lw(a0, MemOperand(sp, 0));
  __ lw(a1, MemOperand(sp, 4));

  __ bind(&slow);
  __ IncrementCounter(&Counters::keyed_load_generic_slow, 1, a0, a1);
  GenerateRuntimeGetProperty(masm);

  __ Ret();
}


void KeyedLoadIC::GenerateString(MacroAssembler* masm) {
  // ra     : return address
  // sp[0]  : key
  // sp[4]  : receiver

  GenerateGeneric(masm);
}


void KeyedLoadIC::GenerateExternalArray(MacroAssembler* masm,
                                        ExternalArrayType array_type) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void KeyedStoreIC::GenerateRuntimeSetProperty(MacroAssembler* masm) {
  // a0     : value
  // ra     : return address
  // sp[0]  : key
  // sp[1]  : receiver
  __ lw(a1, MemOperand(sp, 0));
  __ lw(a3, MemOperand(sp, 4));
//  __ teq(a0, a1, __LINE__);
  __ MultiPush(a0.bit() | a1.bit() | a3.bit());

  __ TailCallRuntime(Runtime::kSetProperty, 3, 1);
}


void KeyedStoreIC::GenerateGeneric(MacroAssembler* masm) {
  // a0     : value
  // ra     : return address
  // sp[0]  : key
  // sp[1]  : receiver
  GenerateRuntimeSetProperty(masm);
}


void KeyedStoreIC::GenerateExternalArray(MacroAssembler* masm,
                                         ExternalArrayType array_type) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


void KeyedLoadIC::GenerateIndexedInterceptor(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- sp[0]  : key
  //  -- sp[4]  : receiver
  // -----------------------------------
  Label slow;

  // Get the key and receiver object from the stack.
  __ lw(a0, MemOperand(sp, 0));
  __ lw(a1, MemOperand(sp, 4));

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(a1, &slow);

  // Check that the key is a smi.
  __ BranchOnNotSmi(a0, &slow);

  // Get the map of the receiver.
  __ lw(a2, FieldMemOperand(a1, HeapObject::kMapOffset));

  // Check that it has indexed interceptor and access checks
  // are not enabled for this object.
  __ lbu(a3, FieldMemOperand(a2, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(kSlowCaseBitFieldMask));
  __ Branch(&slow, ne, a3, Operand(1 << Map::kHasIndexedInterceptor));
  // Everything is fine, call runtime.
  __ Push(a1);  // receiver
  __ Push(a0);  // key

  // Perform tail call to the entry.
  __ TailCallExternalReference(ExternalReference(
        IC_Utility(kKeyedLoadPropertyWithInterceptor)), 2, 1);

  __ bind(&slow);
  GenerateMiss(masm);
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
  // a0    : value
  // a1    : receiver
  // a2    : name
  // ra    : return address

  // Get the receiver from the stack and probe the stub cache.
  Code::Flags flags = Code::ComputeFlags(Code::STORE_IC,
                                         NOT_IN_LOOP,
                                         MONOMORPHIC);
  StubCache::GenerateProbe(masm, flags, a1, a2, a3, no_reg);

  // Cache miss: Jump to runtime.
  GenerateMiss(masm);
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
  // ----------- S t a t e -------------
  //  -- a0    : value
  //  -- a1    : receiver
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------
  //
  // This accepts as a receiver anything JSObject::SetElementsLength accepts
  // (currently anything except for external and pixel arrays which means
  // anything with elements of FixedArray type.), but currently is restricted
  // to JSArray.
  // Value must be a number, but only smis are accepted as the most common case.

  Label miss;

  Register receiver = a1;
  Register value = a0;
  Register scratch = a3;

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver, &miss);

  // Check that the object is a JS array.
  __ GetObjectType(receiver, scratch, scratch);
  __ Branch(&miss, ne, scratch, Operand(JS_ARRAY_TYPE));

  // Check that elements are FixedArray.
  __ lw(scratch, FieldMemOperand(receiver, JSArray::kElementsOffset));
  __ GetObjectType(scratch, scratch, scratch);
  __ Branch(&miss, ne, scratch, Operand(FIXED_ARRAY_TYPE));

  // Check that value is a smi.
  __ BranchOnNotSmi(value, &miss);

  // Prepare tail call to StoreIC_ArrayLength.
  __ push(receiver);
  __ push(value);

  ExternalReference ref = ExternalReference(IC_Utility(kStoreIC_ArrayLength));
  __ TailCallExternalReference(ref, 2, 1);

  __ bind(&miss);

  GenerateMiss(masm);
}

#undef __

} }  // namespace v8::internal

