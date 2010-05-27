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
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- sp[0]  : key
  //  -- sp[4]  : receiver
  // -----------------------------------
  Label slow, failed_allocation;

  // Get the key and receiver object from the stack.
// __ ldm(ia, sp, r0.bit() | r1.bit());
  __ lw(a0, MemOperand(sp, 0));
  __ lw(a1, MemOperand(sp, 4));

  // a0: key
  // a1: receiver object

  // Check that the object isn't a smi
  __ BranchOnSmi(a1, &slow);

  // Check that the key is a smi.
  __ BranchOnNotSmi(a0, &slow);

  // Check that the object is a JS object. Load map into a2.
// __ CompareObjectType(r1, r2, r3, FIRST_JS_OBJECT_TYPE);
// __ b(lt, &slow);
  __ GetObjectType(a1, a2, a3);
  __ Branch(&slow, lt, a3, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that the receiver does not require access checks.  We need
  // to check this explicitly since this generic stub does not perform
  // map checks.
  __ lbu(a3, FieldMemOperand(a2, Map::kBitFieldOffset));
// __ tst(r3, Operand(1 << Map::kIsAccessCheckNeeded));
// __ b(ne, &slow);
  __ And(a3, a3, Operand(1 << Map::kIsAccessCheckNeeded));  // plind, trash a3 here.... OK ??????
  __ Branch(&slow, ne, a3, Operand(zero_reg));

  // Check that the elements array is the appropriate type of
  // ExternalArray.
  // a0: index (as a smi)
  // a1: JSObject
  __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));
  __ lw(a2, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::RootIndexForExternalArrayType(array_type));
// __ cmp(r2, ip);
// __ b(ne, &slow);
  __ Branch(&slow, ne, a2, Operand(t0));

  // Check that the index is in range.
  __ lw(t0, FieldMemOperand(a1, ExternalArray::kLengthOffset));
  __ sra(t1, a0, kSmiTagSize);
// __ cmp(r1, Operand(r0, ASR, kSmiTagSize));  <--- r1 was a bug, now ip in arm code
// __ b(lo, &slow);
  // Unsigned comparison catches both negative and too-large values.
  __ Branch(&slow, Uless, t0, Operand(t1));

  // a0: index (smi)
  // t1: key (un-tagged)
  // a1: elements array
  __ lw(a1, FieldMemOperand(a1, ExternalArray::kExternalPointerOffset));
  // a1: base pointer of external storage

  switch (array_type) {
    case kExternalByteArray:
    // __ ldrsb(r0, MemOperand(r1, r0, LSR, 1));
      __ addu(t0, a1, t1);
      __ lb(a0, MemOperand(t0, 0));
      break;
    case kExternalUnsignedByteArray:
    // __ ldrb(r0, MemOperand(r1, r0, LSR, 1));
      __ addu(t0, a1, t1);
      __ lbu(a0, MemOperand(t0, 0));
      break;
    case kExternalShortArray:
    // __ ldrsh(r0, MemOperand(r1, r0, LSL, 0));
      __ sll(t0, t1, 1);
      __ addu(t0, a1, t0);
      __ lh(a0, MemOperand(t0, 0));
      break;
    case kExternalUnsignedShortArray:
    // __ ldrh(r0, MemOperand(r1, r0, LSL, 0));
      __ sll(t0, t1, 1);
      __ addu(t0, a1, t0);
      __ lhu(a0, MemOperand(t0, 0));
      break;
    case kExternalIntArray:
    case kExternalUnsignedIntArray:
    // __ ldr(r0, MemOperand(r1, r0, LSL, 1));
      __ sll(t0, t1, 2);
      __ addu(t0, a1, t0);
      __ lw(a0, MemOperand(t0, 0));
      break;
    case kExternalFloatArray:
      // if (CpuFeatures::IsSupported(VFP3)) {
      //   CpuFeatures::Scope scope(VFP3);
      // __ add(r0, r1, Operand(r0, LSL, 1));
      // __ vldr(s0, r0, 0);
      if (1) {  // plind -- hack this crap .........................................
        __ sll(t0, t1, 2);
        __ addu(t0, a1, t0);
        __ lwc1(f0, MemOperand(t0, 0));
      } else {
        __ sll(t0, t1, 2);
        __ addu(t0, a1, t0);
        __ lw(a0, MemOperand(t0, 0));
      }
      break;
    default:
      UNREACHABLE();
      break;
  }

  // For integer array types:
  // a0: value
  // For floating-point array type
  // f0: value (if FPU is supported)
  // a0: value (if FPU is not supported)

  if (array_type == kExternalIntArray) {
    // For the Int and UnsignedInt array types, we need to see whether
    // the value can be represented in a Smi. If not, we need to convert
    // it to a HeapNumber.
    Label box_int;
  // __ cmp(r0, Operand(0xC0000000));
  // __ b(mi, &box_int);
    __ Subu(t0, a0, Operand(0xc0000000));  // ...............................................
    __ Branch(&box_int, lt, a0, Operand(zero_reg));  // .... plind ... error here ???????????
    __ sll(v0, a0, kSmiTagSize);
    __ Ret();

    __ bind(&box_int);

  // __ mov(r1, r0);
    // Allocate a HeapNumber for the int and perform int-to-double
    // conversion.
    __ AllocateHeapNumber(v0, a3, t0, &slow);

    // plind hack for FPU instructions
    __ mtc1(a0, f0);
    __ cvt_d_w(f0, f0);
    __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset));
    __ Ret();

// plind - Below is the correct implementation that needs to be translated
    // if (CpuFeatures::IsSupported(FPU)) {
    //   CpuFeatures::Scope scope(FPU);
    //   __ vmov(s0, r1);
    //   __ vcvt_f64_s32(d0, s0);
    //   __ sub(r1, r0, Operand(kHeapObjectTag));
    //   __ vstr(d0, r1, HeapNumber::kValueOffset);
    //   __ Ret();
    // } else {
    //   WriteInt32ToHeapNumberStub stub(r1, r0, r3);
    //   __ TailCallStub(&stub);
    // }
  } else if (array_type == kExternalUnsignedIntArray) {
    // The test is different for unsigned int values. Since we need
    // the value to be in the range of a positive smi, we can't
    // handle either of the top two bits being set in the value.
    
    
    // plind hack to force use of FP instructions ------------------------------------
    
    Label pl_box_int;
    __ And(t0, a0, Operand(0xC0000000));
    __ Branch(&pl_box_int, ne, t0, Operand(zero_reg));
    
    // It can fit in an Smi.
    __ sll(v0, a0, kSmiTagSize);
    __ Ret();
    
    __ bind(&pl_box_int);
    __ AllocateHeapNumber(v0, a1, a2, &slow);
    __ mtc1(a0, f0);
    __ cvt_d_w(f0, f0);  // ........................use 64 bit to get true unsigned 32 .....
    __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset));
    __ Ret();
    
    // if (CpuFeatures::IsSupported(VFP3)) {
    //   CpuFeatures::Scope scope(VFP3);
    //   Label box_int, done;
    //   __ tst(r0, Operand(0xC0000000));
    //   __ b(ne, &box_int);
    // 
    //   __ mov(r0, Operand(r0, LSL, kSmiTagSize));
    //   __ Ret();
    // 
    //   __ bind(&box_int);
    //   __ vmov(s0, r0);
    //   __ AllocateHeapNumber(r0, r1, r2, &slow);
    // 
    //   __ vcvt_f64_u32(d0, s0);
    //   __ sub(r1, r0, Operand(kHeapObjectTag));
    //   __ vstr(d0, r1, HeapNumber::kValueOffset);
    //   __ Ret();
    // } else {
    //   // Check whether unsigned integer fits into smi.
    //   Label box_int_0, box_int_1, done;
    //   __ tst(r0, Operand(0x80000000));
    //   __ b(ne, &box_int_0);
    //   __ tst(r0, Operand(0x40000000));
    //   __ b(ne, &box_int_1);
    // 
    //   // Tag integer as smi and return it.
    //   __ mov(r0, Operand(r0, LSL, kSmiTagSize));
    //   __ Ret();
    // 
    //   __ bind(&box_int_0);
    //   // Integer does not have leading zeros.
    //   GenerateUInt2Double(masm, r0, r1, r2, 0);
    //   __ b(&done);
    // 
    //   __ bind(&box_int_1);
    //   // Integer has one leading zero.
    //   GenerateUInt2Double(masm, r0, r1, r2, 1);
    // 
    //   __ bind(&done);
    //   // Integer was converted to double in registers r0:r1.
    //   // Wrap it into a HeapNumber.
    //   __ AllocateHeapNumber(r2, r3, r5, &slow);
    // 
    //   __ str(r0, FieldMemOperand(r2, HeapNumber::kExponentOffset));
    //   __ str(r1, FieldMemOperand(r2, HeapNumber::kMantissaOffset));
    // 
    //   __ mov(r0, r2);
    // 
    //   __ Ret();
    // }
  } else if (array_type == kExternalFloatArray) {
    // the float (single) value is already in fpu reg f0 (if we use float)
    // plind hack for FPU instructions
    __ AllocateHeapNumber(v0, a1, a2, &slow);
    __ cvt_d_s(f0, f0);
    __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset));
    __ Ret();
    
    
    
    // For the floating-point array type, we need to always allocate a
    // HeapNumber.
    // if (CpuFeatures::IsSupported(VFP3)) {
    //   CpuFeatures::Scope scope(VFP3);
    //   __ AllocateHeapNumber(r0, r1, r2, &slow);
    //   __ vcvt_f64_f32(d0, s0);
    //   __ sub(r1, r0, Operand(kHeapObjectTag));
    //   __ vstr(d0, r1, HeapNumber::kValueOffset);
    //   __ Ret();
    // } else {
    //   __ AllocateHeapNumber(r3, r1, r2, &slow);
    //   // VFP is not available, do manual single to double conversion.
    // 
    //   // r0: floating point value (binary32)
    // 
    //   // Extract mantissa to r1.
    //   __ and_(r1, r0, Operand(kBinary32MantissaMask));
    // 
    //   // Extract exponent to r2.
    //   __ mov(r2, Operand(r0, LSR, kBinary32MantissaBits));
    //   __ and_(r2, r2, Operand(kBinary32ExponentMask >> kBinary32MantissaBits));
    // 
    //   Label exponent_rebiased;
    //   __ teq(r2, Operand(0x00));
    //   __ b(eq, &exponent_rebiased);
    // 
    //   __ teq(r2, Operand(0xff));
    //   __ mov(r2, Operand(0x7ff), LeaveCC, eq);
    //   __ b(eq, &exponent_rebiased);
    // 
    //   // Rebias exponent.
    //   __ add(r2,
    //          r2,
    //          Operand(-kBinary32ExponentBias + HeapNumber::kExponentBias));
    // 
    //   __ bind(&exponent_rebiased);
    //   __ and_(r0, r0, Operand(kBinary32SignMask));
    //   __ orr(r0, r0, Operand(r2, LSL, HeapNumber::kMantissaBitsInTopWord));
    // 
    //   // Shift mantissa.
    //   static const int kMantissaShiftForHiWord =
    //       kBinary32MantissaBits - HeapNumber::kMantissaBitsInTopWord;
    // 
    //   static const int kMantissaShiftForLoWord =
    //       kBitsPerInt - kMantissaShiftForHiWord;
    // 
    //   __ orr(r0, r0, Operand(r1, LSR, kMantissaShiftForHiWord));
    //   __ mov(r1, Operand(r1, LSL, kMantissaShiftForLoWord));
    // 
    //   __ str(r0, FieldMemOperand(r3, HeapNumber::kExponentOffset));
    //   __ str(r1, FieldMemOperand(r3, HeapNumber::kMantissaOffset));
    //   __ mov(r0, r3);
    //   __ Ret();
    // }

  } else {
    // Remaining array-types will all fit in Smi.
    __ sll(v0, a0, kSmiTagSize);
    __ Ret();
  }

  // Slow case: Load name and receiver from stack and jump to runtime.
  __ bind(&slow);
  __ IncrementCounter(&Counters::keyed_load_external_array_slow, 1, a0, a1);
  GenerateRuntimeGetProperty(masm);
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


// Convert int passed in register ival to IEEE-754 single precision
// floating point value and store it into register fval.
// If FPU is available use it for conversion.
static void ConvertIntToFloat(MacroAssembler* masm,
                              Register ival,
                              Register fval,
                              Register scratch1,
                              Register scratch2) {
  // if (CpuFeatures::IsSupported(FPU)) {
  if (1) {  // early init bug, this routine called before CPU::Setup() inits features
    // CpuFeatures::Scope scope(FPU);  // early init bug, this routine called before CPU::Setup() inits features
    // __ vmov(s0, ival);
    // __ vcvt_f32_s32(s0, s0);
    // __ vmov(fval, s0);
    __ mtc1(ival, f0);
    __ cvt_s_w(f0, f0);
    __ mfc1(fval, f0);
  } else {
    // FPU is not available,  do manual conversions.
    
    // Have not converted the Arm code below yet. Force an error.
    // ASSERT(CpuFeatures::IsSupported(FPU)); ..................................................

    // Label not_special, done;
    // // Move sign bit from source to destination.  This works because the sign
    // // bit in the exponent word of the double has the same position and polarity
    // // as the 2's complement sign bit in a Smi.
    // ASSERT(kBinary32SignMask == 0x80000000u);
    // 
    // __ and_(fval, ival, Operand(kBinary32SignMask), SetCC);
    // // Negate value if it is negative.
    // __ rsb(ival, ival, Operand(0), LeaveCC, ne);
    // 
    // // We have -1, 0 or 1, which we treat specially. Register ival contains
    // // absolute value: it is either equal to 1 (special case of -1 and 1),
    // // greater than 1 (not a special case) or less than 1 (special case of 0).
    // __ cmp(ival, Operand(1));
    // __ b(gt, &not_special);
    // 
    // // For 1 or -1 we need to or in the 0 exponent (biased).
    // static const uint32_t exponent_word_for_1 =
    //     kBinary32ExponentBias << kBinary32ExponentShift;
    // 
    // __ orr(fval, fval, Operand(exponent_word_for_1), LeaveCC, eq);
    // __ b(&done);
    // 
    // __ bind(&not_special);
    // // Count leading zeros.
    // // Gets the wrong answer for 0, but we already checked for that case above.
    // Register zeros = scratch2;
    // __ CountLeadingZeros(ival, scratch1, zeros);
    // 
    // // Compute exponent and or it into the exponent register.
    // __ rsb(scratch1,
    //        zeros,
    //        Operand((kBitsPerInt - 1) + kBinary32ExponentBias));
    // 
    // __ orr(fval,
    //        fval,
    //        Operand(scratch1, LSL, kBinary32ExponentShift));
    // 
    // // Shift up the source chopping the top bit off.
    // __ add(zeros, zeros, Operand(1));
    // // This wouldn't work for 1 and -1 as the shift would be 32 which means 0.
    // __ mov(ival, Operand(ival, LSL, zeros));
    // // And the top (top 20 bits).
    // __ orr(fval,
    //        fval,
    //        Operand(ival, LSR, kBitsPerInt - kBinary32MantissaBits));
    // 
    // __ bind(&done);
  }
}


static bool IsElementTypeSigned(ExternalArrayType array_type) {
  switch (array_type) {
    case kExternalByteArray:
    case kExternalShortArray:
    case kExternalIntArray:
      return true;

    case kExternalUnsignedByteArray:
    case kExternalUnsignedShortArray:
    case kExternalUnsignedIntArray:
      return false;

    default:
      UNREACHABLE();
      return false;
  }
}


void KeyedStoreIC::GenerateExternalArray(MacroAssembler* masm,
                                         ExternalArrayType array_type) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- ra     : return address
  //  -- sp[0]  : key
  //  -- sp[1]  : receiver
  // -----------------------------------
  Label slow, check_heap_number;

  // Get the key and the object from the stack.
// __ ldm(ia, sp, r1.bit() | r2.bit());  // r1 = key, r2 = receiver
  __ lw(a1, MemOperand(sp, 0));  // a1 = key.
  __ lw(a2, MemOperand(sp, 4));  // a2 = receiver.

  // Check that the object isn't a smi.
  __ BranchOnSmi(a2, &slow);

  // Check that the object is a JS object. Load map into a3.
// __ CompareObjectType(r2, r3, r4, FIRST_JS_OBJECT_TYPE);
// __ b(le, &slow);
  __ GetObjectType(a2, a3, t0);
  __ Branch(&slow, lt, t0, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that the receiver does not require access checks.  We need
  // to do this because this generic stub does not perform map checks.
// __ ldrb(ip, FieldMemOperand(r3, Map::kBitFieldOffset));
// __ tst(ip, Operand(1 << Map::kIsAccessCheckNeeded));
// __ b(ne, &slow);
  __ lbu(t1, FieldMemOperand(a3, Map::kBitFieldOffset));
  __ And(t1, t1, Operand(1 << Map::kIsAccessCheckNeeded));  // plind, trash t1 here.... OK ??????
  __ Branch(&slow, ne, t1, Operand(zero_reg));

  // Check that the key is a smi.
  __ BranchOnNotSmi(a1, &slow);

  // Check that the elements array is the appropriate type of
  // ExternalArray.
  // a0: value
  // a1: index (smi)
  // a2: object
  __ lw(a2, FieldMemOperand(a2, JSObject::kElementsOffset));
  __ lw(a3, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ LoadRoot(t1, Heap::RootIndexForExternalArrayType(array_type));
// __ cmp(r3, ip);
// __ b(ne, &slow);
  __ Branch(&slow, ne, a3, Operand(t1));

  // Check that the index is in range.
// __ mov(r1, Operand(r1, ASR, kSmiTagSize));  // Untag the index.
  __ sra(a1, a1, kSmiTagSize);  // Untag the index.
  __ lw(t1, FieldMemOperand(a2, ExternalArray::kLengthOffset));
// __ cmp(r1, ip);
// __ b(hs, &slow);
  // Unsigned comparison catches both negative and too-large values.
  __ Branch(&slow, Ugreater_equal, a1, Operand(t1));

  // Handle both smis and HeapNumbers in the fast path. Go to the
  // runtime for all other kinds of values.
  // a0: value
  // a1: index (integer)
  // a2: array
  __ BranchOnNotSmi(a0, &check_heap_number);
  __ sra(a3, a0, kSmiTagSize);  // Untag the value.
  __ lw(a2, FieldMemOperand(a2, ExternalArray::kExternalPointerOffset));

  // a1: index (integer)
  // a2: base pointer of external storage
  // a3: value (integer)
  switch (array_type) {
    case kExternalByteArray:
    case kExternalUnsignedByteArray:
      // __ strb(r3, MemOperand(r2, r1, LSL, 0));
      __ addu(t0, a2, a1);
      __ sb(a3, MemOperand(t0, 0));
      break;
    case kExternalShortArray:
    case kExternalUnsignedShortArray:
      // __ strh(r3, MemOperand(r2, r1, LSL, 1));
      __ sll(t0, a1, 1);
      __ addu(t0, a2, t0);
      __ sh(a3, MemOperand(t0, 0));
      break;
    case kExternalIntArray:
    case kExternalUnsignedIntArray:
      // __ str(r3, MemOperand(r2, r1, LSL, 2));
      __ sll(t0, a1, 2);
      __ addu(t0, a2, t0);
      __ sw(a3, MemOperand(t0, 0));
      break;
    case kExternalFloatArray:
      // Need to perform int-to-float conversion.
      ConvertIntToFloat(masm, a3, t0, t1, t2);
      __ sll(t1, a1, 2);
      __ addu(t1, a2, t1);
      __ sw(t0, MemOperand(t1, 0));
      break;
    default:
      UNREACHABLE();
      break;
  }

  // a0: original value
  __ mov(v0, a0);
  __ Ret();


  // a0: value
  // a1: index (integer)
  // a2: external array object
  __ bind(&check_heap_number);
  // __ CompareObjectType(r0, r3, r4, HEAP_NUMBER_TYPE);
  // __ b(ne, &slow);
  __ GetObjectType(a0, a3, t0);
  __ Branch(&slow, ne, t0, Operand(HEAP_NUMBER_TYPE));

  __ lw(a2, FieldMemOperand(a2, ExternalArray::kExternalPointerOffset));

  // The WebGL specification leaves the behavior of storing NaN and
  // +/-Infinity into integer arrays basically undefined. For more
  // reproducible behavior, convert these to zero.
  
  // if (CpuFeatures::IsSupported(FPU)) {
  if (1) {  // early init bug, this routine called before CPU::Setup() inits features
    
    // CpuFeatures::Scope scope(FPU);  // early init bug, this routine called before CPU::Setup() inits features

    __ ldc1(f0, MemOperand(a0, HeapNumber::kValueOffset - kHeapObjectTag));

    if (array_type == kExternalFloatArray) {
      // __ vcvt_f32_f64(s0, d0);
      // __ vmov(r3, s0);
      __ cvt_s_d(f0, f0);
      // __ mfc1(a3, f0);
      // __ str(r3, MemOperand(r2, r1, LSL, 2));  ....can do better with swc1
      __ sll(t0, a1, 2);
      __ addu(t0, a2, t0);
      __ swc1(f0, MemOperand(t0, 0));
    } else {
      Label done;

        // // Need to perform float-to-int conversion.
        // // Test for NaN.
        // __ vcmp(d0, d0);
        // // Move vector status bits to normal status bits.
        // __ vmrs(v8::internal::pc);
        // __ mov(r3, Operand(0), LeaveCC, vs);  // NaN converts to 0
        // __ b(vs, &done);
        // 
        // // Test whether exponent equal to 0x7FF (infinity or NaN)
        // __ vmov(r4, r3, d0);
        // __ mov(r5, Operand(0x7FF00000));
        // __ and_(r3, r3, Operand(r5));
        // __ teq(r3, Operand(r5));
        // __ mov(r3, Operand(0), LeaveCC, eq);
        // 
        // // Not infinity or NaN simply convert to int
        // if (IsElementTypeSigned(array_type)) {
        //   __ vcvt_s32_f64(s0, d0, ne);
        // } else {
        //   __ vcvt_u32_f64(s0, d0, ne);
        // }
        // 
        // __ vmov(r3, s0, ne);

      // Need to perform float-to-int conversion.
      // Test whether exponent equal to 0x7FF (infinity or NaN).
      
      __ mfc1(a3, f0);  // Move exponent word of double to a3 (as raw bits).
      __ li(t0, Operand(0x7FF00000));
      __ And(a3, a3, Operand(t0));
      __ Branch(false, &done, eq, a3, Operand(zero_reg));
      __ mov(a3, zero_reg);  // In delay slot.

      // Not infinity or NaN simply convert to int.
      if (IsElementTypeSigned(array_type)) {
        // __ vcvt_s32_f64(s0, d0, ne);
        __ cvt_w_d(f0, f0);
        __ mfc1(a3, f0);
      } else {
        // __ vcvt_u32_f64(s0, d0, ne);
        __ cvt_l_d(f0, f0);  // Convert double to 64-bit int.
        __ mfc1(a3, f0);  // Keep the LS 32-bits (little endian).
      }

      // a1: index (integer)
      // a2: external array base address
      // a3: HeapNumber converted to integer
      __ bind(&done);
      switch (array_type) {
        case kExternalByteArray:
        case kExternalUnsignedByteArray:
          // __ strb(r3, MemOperand(r2, r1, LSL, 0));
          __ addu(t0, a2, a1);
          __ sb(a3, MemOperand(t0, 0));
          break;
        case kExternalShortArray:
        case kExternalUnsignedShortArray:
          // __ strh(r3, MemOperand(r2, r1, LSL, 1));
          __ sll(t0, a1, 1);
          __ addu(t0, a2, t0);
          __ sh(a3, MemOperand(t0, 0));
          break;
        case kExternalIntArray:
        case kExternalUnsignedIntArray:
          // __ str(r3, MemOperand(r2, r1, LSL, 2));
          __ sll(t0, a1, 2);
          __ addu(t0, a2, t0);
          __ sw(a3, MemOperand(t0, 0));
          break;
        default:
          UNREACHABLE();
          break;
      }
    }

    // a0: original value
    __ mov(v0, a0);
    __ Ret();
  } else {
    // FPU is not available,  do manual conversions.
    
    // Have not converted the Arm code below yet. Force an error.
    // ASSERT(CpuFeatures::IsSupported(FPU)); ..................................................


    // __ ldr(r3, FieldMemOperand(r0, HeapNumber::kExponentOffset));
    // __ ldr(r4, FieldMemOperand(r0, HeapNumber::kMantissaOffset));
    // 
    // if (array_type == kExternalFloatArray) {
    //   Label done, nan_or_infinity_or_zero;
    //   static const int kMantissaInHiWordShift =
    //       kBinary32MantissaBits - HeapNumber::kMantissaBitsInTopWord;
    // 
    //   static const int kMantissaInLoWordShift =
    //       kBitsPerInt - kMantissaInHiWordShift;
    // 
    //   // Test for all special exponent values: zeros, subnormal numbers, NaNs
    //   // and infinities. All these should be converted to 0.
    //   __ mov(r5, Operand(HeapNumber::kExponentMask));
    //   __ and_(r6, r3, Operand(r5), SetCC);
    //   __ b(eq, &nan_or_infinity_or_zero);
    // 
    //   __ teq(r6, Operand(r5));
    //   __ mov(r6, Operand(kBinary32ExponentMask), LeaveCC, eq);
    //   __ b(eq, &nan_or_infinity_or_zero);
    // 
    //   // Rebias exponent.
    //   __ mov(r6, Operand(r6, LSR, HeapNumber::kExponentShift));
    //   __ add(r6,
    //          r6,
    //          Operand(kBinary32ExponentBias - HeapNumber::kExponentBias));
    // 
    //   __ cmp(r6, Operand(kBinary32MaxExponent));
    //   __ and_(r3, r3, Operand(HeapNumber::kSignMask), LeaveCC, gt);
    //   __ orr(r3, r3, Operand(kBinary32ExponentMask), LeaveCC, gt);
    //   __ b(gt, &done);
    // 
    //   __ cmp(r6, Operand(kBinary32MinExponent));
    //   __ and_(r3, r3, Operand(HeapNumber::kSignMask), LeaveCC, lt);
    //   __ b(lt, &done);
    // 
    //   __ and_(r7, r3, Operand(HeapNumber::kSignMask));
    //   __ and_(r3, r3, Operand(HeapNumber::kMantissaMask));
    //   __ orr(r7, r7, Operand(r3, LSL, kMantissaInHiWordShift));
    //   __ orr(r7, r7, Operand(r4, LSR, kMantissaInLoWordShift));
    //   __ orr(r3, r7, Operand(r6, LSL, kBinary32ExponentShift));
    // 
    //   __ bind(&done);
    //   __ str(r3, MemOperand(r2, r1, LSL, 2));
    //   __ Ret();
    // 
    //   __ bind(&nan_or_infinity_or_zero);
    //   __ and_(r7, r3, Operand(HeapNumber::kSignMask));
    //   __ and_(r3, r3, Operand(HeapNumber::kMantissaMask));
    //   __ orr(r6, r6, r7);
    //   __ orr(r6, r6, Operand(r3, LSL, kMantissaInHiWordShift));
    //   __ orr(r3, r6, Operand(r4, LSR, kMantissaInLoWordShift));
    //   __ b(&done);
    // } else {
    //   bool is_signed_type  = IsElementTypeSigned(array_type);
    //   int meaningfull_bits = is_signed_type ? (kBitsPerInt - 1) : kBitsPerInt;
    //   int32_t min_value    = is_signed_type ? 0x80000000 : 0x00000000;
    // 
    //   Label done, sign;
    // 
    //   // Test for all special exponent values: zeros, subnormal numbers, NaNs
    //   // and infinities. All these should be converted to 0.
    //   __ mov(r5, Operand(HeapNumber::kExponentMask));
    //   __ and_(r6, r3, Operand(r5), SetCC);
    //   __ mov(r3, Operand(0), LeaveCC, eq);
    //   __ b(eq, &done);
    // 
    //   __ teq(r6, Operand(r5));
    //   __ mov(r3, Operand(0), LeaveCC, eq);
    //   __ b(eq, &done);
    // 
    //   // Unbias exponent.
    //   __ mov(r6, Operand(r6, LSR, HeapNumber::kExponentShift));
    //   __ sub(r6, r6, Operand(HeapNumber::kExponentBias), SetCC);
    //   // If exponent is negative than result is 0.
    //   __ mov(r3, Operand(0), LeaveCC, mi);
    //   __ b(mi, &done);
    // 
    //   // If exponent is too big than result is minimal value
    //   __ cmp(r6, Operand(meaningfull_bits - 1));
    //   __ mov(r3, Operand(min_value), LeaveCC, ge);
    //   __ b(ge, &done);
    // 
    //   __ and_(r5, r3, Operand(HeapNumber::kSignMask), SetCC);
    //   __ and_(r3, r3, Operand(HeapNumber::kMantissaMask));
    //   __ orr(r3, r3, Operand(1u << HeapNumber::kMantissaBitsInTopWord));
    // 
    //   __ rsb(r6, r6, Operand(HeapNumber::kMantissaBitsInTopWord), SetCC);
    //   __ mov(r3, Operand(r3, LSR, r6), LeaveCC, pl);
    //   __ b(pl, &sign);
    // 
    //   __ rsb(r6, r6, Operand(0));
    //   __ mov(r3, Operand(r3, LSL, r6));
    //   __ rsb(r6, r6, Operand(meaningfull_bits));
    //   __ orr(r3, r3, Operand(r4, LSR, r6));
    // 
    //   __ bind(&sign);
    //   __ teq(r5, Operand(0));
    //   __ rsb(r3, r3, Operand(0), LeaveCC, ne);
    // 
    //   __ bind(&done);
    //   switch (array_type) {
    //     case kExternalByteArray:
    //     case kExternalUnsignedByteArray:
    //       __ strb(r3, MemOperand(r2, r1, LSL, 0));
    //       break;
    //     case kExternalShortArray:
    //     case kExternalUnsignedShortArray:
    //       __ strh(r3, MemOperand(r2, r1, LSL, 1));
    //       break;
    //     case kExternalIntArray:
    //     case kExternalUnsignedIntArray:
    //       __ str(r3, MemOperand(r2, r1, LSL, 2));
    //       break;
    //     default:
    //       UNREACHABLE();
    //       break;
    //   }
    // }
  }

  // Slow case: call runtime.
  __ bind(&slow);
  GenerateRuntimeSetProperty(masm);
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

