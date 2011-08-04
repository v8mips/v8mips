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
#include "code-stubs.h"
#include "ic-inl.h"
#include "runtime.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {


// ----------------------------------------------------------------------------
// Static IC stub generators.
//

#define __ ACCESS_MASM(masm)


static void GenerateGlobalInstanceTypeCheck(MacroAssembler* masm,
                                            Register type,
                                            Label* global_object) {
  // Register usage:
  //   type: holds the receiver instance type on entry.
  __ Branch(global_object, eq, type, Operand(JS_GLOBAL_OBJECT_TYPE));
  __ Branch(global_object, eq, type, Operand(JS_BUILTINS_OBJECT_TYPE));
  __ Branch(global_object, eq, type, Operand(JS_GLOBAL_PROXY_TYPE));
}


// Generated code falls through if the receiver is a regular non-global
// JS object with slow properties and no interceptors.
static void GenerateStringDictionaryReceiverCheck(MacroAssembler* masm,
                                                  Register receiver,
                                                  Register elements,
                                                  Register scratch0,
                                                  Register scratch1,
                                                  Label* miss) {
  // Register usage:
  //   receiver: holds the receiver on entry and is unchanged.
  //   elements: holds the property dictionary on fall through.
  // Scratch registers:
  //   scratch0: used to holds the receiver map.
  //   scratch1: used to holds the receiver instance type, receiver bit mask
  //     and elements map.

  // Check that the receiver isn't a smi.
  __ JumpIfSmi(receiver, miss);

  // Check that the receiver is a valid JS object.
  __ GetObjectType(receiver, scratch0, scratch1);
  __ Branch(miss, lt, scratch1, Operand(FIRST_JS_OBJECT_TYPE));

  // If this assert fails, we have to check upper bound too.
  ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);

  GenerateGlobalInstanceTypeCheck(masm, scratch1, miss);

  // Check that the global object does not require access checks.
  __ lbu(scratch1, FieldMemOperand(scratch0, Map::kBitFieldOffset));
  __ And(scratch1, scratch1, Operand((1 << Map::kIsAccessCheckNeeded) |
                           (1 << Map::kHasNamedInterceptor)));
  __ Branch(miss, ne, scratch1, Operand(zero_reg));

  __ lw(elements, FieldMemOperand(receiver, JSObject::kPropertiesOffset));
  __ lw(scratch1, FieldMemOperand(elements, HeapObject::kMapOffset));
  __ LoadRoot(scratch0, Heap::kHashTableMapRootIndex);
  __ Branch(miss, ne, scratch1, Operand(scratch0));
}


// Probe the string dictionary in the |elements| register. Jump to the
// |done| label if a property with the given name is found. Jump to
// the |miss| label otherwise.
static void GenerateStringDictionaryProbes(MacroAssembler* masm,
                                           Label* miss,
                                           Label* done,
                                           Register elements,
                                           Register name,
                                           Register scratch1,
                                           Register scratch2) {
  // Assert that name contains a string.
  if (FLAG_debug_code) __ AbortIfNotString(name);

  // Compute the capacity mask.
  const int kCapacityOffset = StringDictionary::kHeaderSize +
      StringDictionary::kCapacityIndex * kPointerSize;
  __ lw(scratch1, FieldMemOperand(elements, kCapacityOffset));
  __ sra(scratch1, scratch1, kSmiTagSize);  // Convert smi to int.
  __ Subu(scratch1, scratch1, Operand(1));

  const int kElementsStartOffset = StringDictionary::kHeaderSize +
      StringDictionary::kElementsStartIndex * kPointerSize;

  // Generate an unrolled loop that performs a few probes before
  // giving up. Measurements done on Gmail indicate that 2 probes
  // cover ~93% of loads from dictionaries.
  static const int kProbes = 4;
  for (int i = 0; i < kProbes; i++) {
    // Compute the masked index: (hash + i + i * i) & mask.
    __ lw(scratch2, FieldMemOperand(name, String::kHashFieldOffset));
    if (i > 0) {
      // Add the probe offset (i + i * i) left shifted to avoid right shifting
      // the hash in a separate instruction. The value hash + i + i * i is right
      // shifted in the following and instruction.
      ASSERT(StringDictionary::GetProbeOffset(i) <
             1 << (32 - String::kHashFieldOffset));
      __ Addu(scratch2, scratch2, Operand(
          StringDictionary::GetProbeOffset(i) << String::kHashShift));
    }

    __ srl(scratch2, scratch2, String::kHashShift);
    __ And(scratch2, scratch1, scratch2);

    // Scale the index by multiplying by the element size.
    ASSERT(StringDictionary::kEntrySize == 3);
    __ sll(at, scratch2, 1);  // at = scratch2 * 2.
    __ addu(scratch2, scratch2, at);  // scratch2 = scratch2 * 3.

    // Check if the key is identical to the name.
    __ sll(at, scratch2, 2);
    __ addu(scratch2, elements, at);
    __ lw(at, FieldMemOperand(scratch2, kElementsStartOffset));
    if (i != kProbes - 1) {
      __ Branch(done, eq, name, Operand(at));
    } else {
      __ Branch(miss, ne, name, Operand(at));
    }
  }
}


// Helper function used from LoadIC/CallIC GenerateNormal.
//
// elements: Property dictionary. It is not clobbered if a jump to the miss
//           label is done.
// name:     Property name. It is not clobbered if a jump to the miss label is
//           done
// result:   Register for the result. It is only updated if a jump to the miss
//           label is not done. Can be the same as elements or name clobbering
//           one of these in the case of not jumping to the miss label.
// The two scratch registers need to be different from elements, name and
// result.
// The generated code assumes that the receiver has slow properties,
// is not a global object and does not have interceptors.
// The address returned from GenerateStringDictionaryProbes() in scratch2
// is used.
static void GenerateDictionaryLoad(MacroAssembler* masm,
                                   Label* miss,
                                   Register elements,
                                   Register name,
                                   Register result,
                                   Register scratch1,
                                   Register scratch2) {
  // Main use of the scratch registers.
  // scratch1: Used as temporary and to hold the capacity of the property
  //           dictionary.
  // scratch2: Used as temporary.
  Label done;

  // Probe the dictionary.
  GenerateStringDictionaryProbes(masm,
                                 miss,
                                 &done,
                                 elements,
                                 name,
                                 scratch1,
                                 scratch2);

  // If probing finds an entry check that the value is a normal
  // property.
  __ bind(&done);  // scratch2 == elements + 4 * index.
  const int kElementsStartOffset = StringDictionary::kHeaderSize +
      StringDictionary::kElementsStartIndex * kPointerSize;
  const int kDetailsOffset = kElementsStartOffset + 2 * kPointerSize;
  __ lw(scratch1, FieldMemOperand(scratch2, kDetailsOffset));
  __ And(at,
         scratch1,
         Operand(PropertyDetails::TypeField::mask() << kSmiTagSize));
  __ Branch(miss, ne, at, Operand(zero_reg));

  // Get the value at the masked, scaled index and return.
  __ lw(result,
        FieldMemOperand(scratch2, kElementsStartOffset + 1 * kPointerSize));
}


// Helper function used from StoreIC::GenerateNormal.
//
// elements: Property dictionary. It is not clobbered if a jump to the miss
//           label is done.
// name:     Property name. It is not clobbered if a jump to the miss label is
//           done
// value:    The value to store.
// The two scratch registers need to be different from elements, name and
// result.
// The generated code assumes that the receiver has slow properties,
// is not a global object and does not have interceptors.
// The address returned from GenerateStringDictionaryProbes() in scratch2
// is used.
static void GenerateDictionaryStore(MacroAssembler* masm,
                                    Label* miss,
                                    Register elements,
                                    Register name,
                                    Register value,
                                    Register scratch1,
                                    Register scratch2) {
  // Main use of the scratch registers.
  // scratch1: Used as temporary and to hold the capacity of the property
  //           dictionary.
  // scratch2: Used as temporary.
  Label done;

  // Probe the dictionary.
  GenerateStringDictionaryProbes(masm,
                                 miss,
                                 &done,
                                 elements,
                                 name,
                                 scratch1,
                                 scratch2);

  // If probing finds an entry in the dictionary check that the value
  // is a normal property that is not read only.
  __ bind(&done);  // scratch2 == elements + 4 * index
  const int kElementsStartOffset = StringDictionary::kHeaderSize +
      StringDictionary::kElementsStartIndex * kPointerSize;
  const int kDetailsOffset = kElementsStartOffset + 2 * kPointerSize;
  const int kTypeAndReadOnlyMask
      = (PropertyDetails::TypeField::mask() |
         PropertyDetails::AttributesField::encode(READ_ONLY)) << kSmiTagSize;
  __ lw(scratch1, FieldMemOperand(scratch2, kDetailsOffset));
  __ And(at, scratch1, Operand(kTypeAndReadOnlyMask));
  __ Branch(miss, ne, at, Operand(zero_reg));

  // Store the value at the masked, scaled index and return.
  const int kValueOffset = kElementsStartOffset + kPointerSize;
  __ Addu(scratch2, scratch2, Operand(kValueOffset - kHeapObjectTag));
  __ sw(value, MemOperand(scratch2));

  // Update the write barrier. Make sure not to clobber the value.
  __ mov(scratch1, value);
  __ RecordWrite(elements, scratch2, scratch1);
}


static void GenerateNumberDictionaryLoad(MacroAssembler* masm,
                                         Label* miss,
                                         Register elements,
                                         Register key,
                                         Register result,
                                         Register reg0,
                                         Register reg1,
                                         Register reg2) {
  // Register use:
  //
  // elements - holds the slow-case elements of the receiver on entry.
  //            Unchanged unless 'result' is the same register.
  //
  // key      - holds the smi key on entry.
  //            Unchanged unless 'result' is the same register.
  //
  //
  // result   - holds the result on exit if the load succeeded.
  //            Allowed to be the same as 'key' or 'result'.
  //            Unchanged on bailout so 'key' or 'result' can be used
  //            in further computation.
  //
  // Scratch registers:
  //
  // reg0 - holds the untagged key on entry and holds the hash once computed.
  //
  // reg1 - Used to hold the capacity mask of the dictionary.
  //
  // reg2 - Used for the index into the dictionary.
  // at   - Temporary (avoid MacroAssembler instructions also using 'at').
  Label done;

  // Compute the hash code from the untagged key.  This must be kept in sync
  // with ComputeIntegerHash in utils.h.
  //
  // hash = ~hash + (hash << 15);
  __ nor(reg1, reg0, zero_reg);
  __ sll(at, reg0, 15);
  __ addu(reg0, reg1, at);

  // hash = hash ^ (hash >> 12);
  __ srl(at, reg0, 12);
  __ xor_(reg0, reg0, at);

  // hash = hash + (hash << 2);
  __ sll(at, reg0, 2);
  __ addu(reg0, reg0, at);

  // hash = hash ^ (hash >> 4);
  __ srl(at, reg0, 4);
  __ xor_(reg0, reg0, at);

  // hash = hash * 2057;
  __ li(reg1, Operand(2057));
  __ mul(reg0, reg0, reg1);

  // hash = hash ^ (hash >> 16);
  __ srl(at, reg0, 16);
  __ xor_(reg0, reg0, at);

  // Compute the capacity mask.
  __ lw(reg1, FieldMemOperand(elements, NumberDictionary::kCapacityOffset));
  __ sra(reg1, reg1, kSmiTagSize);
  __ Subu(reg1, reg1, Operand(1));

  // Generate an unrolled loop that performs a few probes before giving up.
  static const int kProbes = 4;
  for (int i = 0; i < kProbes; i++) {
    // Use reg2 for index calculations and keep the hash intact in reg0.
    __ mov(reg2, reg0);
    // Compute the masked index: (hash + i + i * i) & mask.
    if (i > 0) {
      __ Addu(reg2, reg2, Operand(NumberDictionary::GetProbeOffset(i)));
    }
    __ and_(reg2, reg2, reg1);

    // Scale the index by multiplying by the element size.
    ASSERT(NumberDictionary::kEntrySize == 3);
    __ sll(at, reg2, 1);  // 2x.
    __ addu(reg2, reg2, at);  // reg2 = reg2 * 3.

    // Check if the key is identical to the name.
    __ sll(at, reg2, kPointerSizeLog2);
    __ addu(reg2, elements, at);

    __ lw(at, FieldMemOperand(reg2, NumberDictionary::kElementsStartOffset));
    if (i != kProbes - 1) {
      __ Branch(&done, eq, key, Operand(at));
    } else {
      __ Branch(miss, ne, key, Operand(at));
    }
  }

  __ bind(&done);
  // Check that the value is a normal property.
  // reg2: elements + (index * kPointerSize)
  const int kDetailsOffset =
      NumberDictionary::kElementsStartOffset + 2 * kPointerSize;
  __ lw(reg1, FieldMemOperand(reg2, kDetailsOffset));
  __ And(at, reg1, Operand(Smi::FromInt(PropertyDetails::TypeField::mask())));
  __ Branch(miss, ne, at, Operand(zero_reg));

  // Get the value at the masked, scaled index and return.
  const int kValueOffset =
      NumberDictionary::kElementsStartOffset + kPointerSize;
  __ lw(result, FieldMemOperand(reg2, kValueOffset));
}


void LoadIC::GenerateArrayLength(MacroAssembler* masm) {
  // a2    : name
  // ra    : return address
  // a0    : receiver
  // sp[0] : receiver

  Label miss;

  StubCompiler::GenerateLoadArrayLength(masm, a0, a3, &miss);
  __ bind(&miss);
  StubCompiler::GenerateLoadMiss(masm, Code::LOAD_IC);
}


void LoadIC::GenerateStringLength(MacroAssembler* masm, bool support_wrappers) {
  // a2    : name
  // lr    : return address
  // a0    : receiver
  // sp[0] : receiver
  Label miss;

  StubCompiler::GenerateLoadStringLength(masm, a0, a1, a3, &miss,
                                         support_wrappers);
  // Cache miss: Jump to runtime.
  __ bind(&miss);
  StubCompiler::GenerateLoadMiss(masm, Code::LOAD_IC);
}


void LoadIC::GenerateFunctionPrototype(MacroAssembler* masm) {
  // a2    : name
  // lr    : return address
  // a0    : receiver
  // sp[0] : receiver

  Label miss;

  StubCompiler::GenerateLoadFunctionPrototype(masm, a0, a1, a3, &miss);
  __ bind(&miss);
  StubCompiler::GenerateLoadMiss(masm, Code::LOAD_IC);
}


// Checks the receiver for special cases (value type, slow case bits).
// Falls through for regular JS object.
static void GenerateKeyedLoadReceiverCheck(MacroAssembler* masm,
                                           Register receiver,
                                           Register map,
                                           Register scratch,
                                           int interceptor_bit,
                                           Label* slow) {
  // Check that the object isn't a smi.
  __ JumpIfSmi(receiver, slow);
  // Get the map of the receiver.
  __ lw(map, FieldMemOperand(receiver, HeapObject::kMapOffset));
  // Check bit field.
  __ lbu(scratch, FieldMemOperand(map, Map::kBitFieldOffset));
  __ And(at, scratch, Operand(KeyedLoadIC::kSlowCaseBitFieldMask));
  __ Branch(slow, ne, at, Operand(zero_reg));
  // Check that the object is some kind of JS object EXCEPT JS Value type.
  // In the case that the object is a value-wrapper object,
  // we enter the runtime system to make sure that indexing into string
  // objects work as intended.
  ASSERT(JS_OBJECT_TYPE > JS_VALUE_TYPE);
  __ lbu(scratch, FieldMemOperand(map, Map::kInstanceTypeOffset));
  __ Branch(slow, lt, scratch, Operand(JS_OBJECT_TYPE));
}


// Loads an indexed element from a fast case array.
// If not_fast_array is NULL, doesn't perform the elements map check.
static void GenerateFastArrayLoad(MacroAssembler* masm,
                                  Register receiver,
                                  Register key,
                                  Register elements,
                                  Register scratch1,
                                  Register scratch2,
                                  Register result,
                                  Label* not_fast_array,
                                  Label* out_of_range) {
  // Register use:
  //
  // receiver - holds the receiver on entry.
  //            Unchanged unless 'result' is the same register.
  //
  // key      - holds the smi key on entry.
  //            Unchanged unless 'result' is the same register.
  //
  // elements - holds the elements of the receiver on exit.
  //
  // result   - holds the result on exit if the load succeeded.
  //            Allowed to be the the same as 'receiver' or 'key'.
  //            Unchanged on bailout so 'receiver' and 'key' can be safely
  //            used by further computation.
  //
  // Scratch registers:
  //
  // scratch1 - used to hold elements map and elements length.
  //            Holds the elements map if not_fast_array branch is taken.
  //
  // scratch2 - used to hold the loaded value.

  __ lw(elements, FieldMemOperand(receiver, JSObject::kElementsOffset));
  if (not_fast_array != NULL) {
    // Check that the object is in fast mode (not dictionary).
    __ lw(scratch1, FieldMemOperand(elements, HeapObject::kMapOffset));
    __ LoadRoot(at, Heap::kFixedArrayMapRootIndex);
    __ Branch(not_fast_array, ne, scratch1, Operand(at));
  } else {
    __ AssertFastElements(elements);
  }

  // Check that the key (index) is within bounds.
  __ lw(scratch1, FieldMemOperand(elements, FixedArray::kLengthOffset));
  __ Branch(out_of_range, hs, key, Operand(scratch1));

  // Fast case: Do the load.
  __ Addu(scratch1, elements,
          Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  // The key is a smi.
  ASSERT(kSmiTag == 0 && kSmiTagSize < kPointerSizeLog2);
  __ sll(at, key, kPointerSizeLog2 - kSmiTagSize);
  __ addu(at, at, scratch1);
  __ lw(scratch2, MemOperand(at));

  __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
  // In case the loaded value is the_hole we have to consult GetProperty
  // to ensure the prototype chain is searched.
  __ Branch(out_of_range, eq, scratch2, Operand(at));
  __ mov(result, scratch2);
}


// Checks whether a key is an array index string or a symbol string.
// Falls through if a key is a symbol.
static void GenerateKeyStringCheck(MacroAssembler* masm,
                                   Register key,
                                   Register map,
                                   Register hash,
                                   Label* index_string,
                                   Label* not_symbol) {
  // The key is not a smi.
  // Is it a string?
  __ GetObjectType(key, map, hash);
  __ Branch(not_symbol, ge, hash, Operand(FIRST_NONSTRING_TYPE));

  // Is the string an array index, with cached numeric value?
  __ lw(hash, FieldMemOperand(key, String::kHashFieldOffset));
  __ And(at, hash, Operand(String::kContainsCachedArrayIndexMask));
  __ Branch(index_string, eq, at, Operand(zero_reg));

  // Is the string a symbol?
  // map: key map
  __ lbu(hash, FieldMemOperand(map, Map::kInstanceTypeOffset));
  ASSERT(kSymbolTag != 0);
  __ And(at, hash, Operand(kIsSymbolMask));
  __ Branch(not_symbol, eq, at, Operand(zero_reg));
}


// Defined in ic.cc.
Object* CallIC_Miss(Arguments args);

// The generated code does not accept smi keys.
// The generated code falls through if both probes miss.
static void GenerateMonomorphicCacheProbe(MacroAssembler* masm,
                                          int argc,
                                          Code::Kind kind) {
  // ----------- S t a t e -------------
  //  -- a1    : receiver
  //  -- a2    : name
  // -----------------------------------
  Label number, non_number, non_string, boolean, probe, miss;

  // Probe the stub cache.
  Code::Flags flags = Code::ComputeFlags(kind,
                                         NOT_IN_LOOP,
                                         MONOMORPHIC,
                                         Code::kNoExtraICState,
                                         NORMAL,
                                         argc);
  StubCache::GenerateProbe(masm, flags, a1, a2, a3, t0, t1);

  // If the stub cache probing failed, the receiver might be a value.
  // For value objects, we use the map of the prototype objects for
  // the corresponding JSValue for the cache and that is what we need
  // to probe.
  //
  // Check for number.
  __ JumpIfSmi(a1, &number, t1);
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
  StubCache::GenerateProbe(masm, flags, a1, a2, a3, t0, t1);

  __ bind(&miss);
}


static void GenerateFunctionTailCall(MacroAssembler* masm,
                                     int argc,
                                     Label* miss,
                                     Register scratch) {
  // a1: function

  // Check that the value isn't a smi.
  __ JumpIfSmi(a1, miss);

  // Check that the value is a JSFunction.
  __ GetObjectType(a1, scratch, scratch);
  __ Branch(miss, ne, scratch, Operand(JS_FUNCTION_TYPE));

  // Invoke the function.
  ParameterCount actual(argc);
  __ InvokeFunction(a1, actual, JUMP_FUNCTION);
}


static void GenerateCallNormal(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------
  Label miss;

  // Get the receiver of the function from the stack into a1.
  __ lw(a1, MemOperand(sp, argc * kPointerSize));

  GenerateStringDictionaryReceiverCheck(masm, a1, a0, a3, t0, &miss);

  // a0: elements
  // Search the dictionary - put result in register a1.
  GenerateDictionaryLoad(masm, &miss, a0, a2, a1, a3, t0);

  GenerateFunctionTailCall(masm, argc, &miss, t0);

  // Cache miss: Jump to runtime.
  __ bind(&miss);
}


static void GenerateCallMiss(MacroAssembler* masm, int argc, IC::UtilityId id) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  if (id == IC::kCallIC_Miss) {
    __ IncrementCounter(&Counters::call_miss, 1, a3, t0);
  } else {
    __ IncrementCounter(&Counters::keyed_call_miss, 1, a3, t0);
  }

  // Get the receiver of the function from the stack.
  __ lw(a3, MemOperand(sp, argc*kPointerSize));

  __ EnterInternalFrame();

  // Push the receiver and the name of the function.
  __ Push(a3, a2);

  // Call the entry.
  __ li(a0, Operand(2));
  __ li(a1, Operand(ExternalReference(IC_Utility(id))));

  CEntryStub stub(1);
  __ CallStub(&stub);

  // Move result to a1 and leave the internal frame.
  __ mov(a1, v0);
  __ LeaveInternalFrame();

  // Check if the receiver is a global object of some sort.
  // This can happen only for regular CallIC but not KeyedCallIC.
  if (id == IC::kCallIC_Miss) {
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
    __ bind(&invoke);
  }
  // Invoke the function.
  ParameterCount actual(argc);
  __ InvokeFunction(a1, actual, JUMP_FUNCTION);
}


void CallIC::GenerateMiss(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  GenerateCallMiss(masm, argc, IC::kCallIC_Miss);
}


void CallIC::GenerateMegamorphic(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  // Get the receiver of the function from the stack into a1.
  __ lw(a1, MemOperand(sp, argc * kPointerSize));
  GenerateMonomorphicCacheProbe(masm, argc, Code::CALL_IC);
  GenerateMiss(masm, argc);
}


void CallIC::GenerateNormal(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  GenerateCallNormal(masm, argc);
  GenerateMiss(masm, argc);
}


void KeyedCallIC::GenerateMiss(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  GenerateCallMiss(masm, argc, IC::kKeyedCallIC_Miss);
}


void KeyedCallIC::GenerateMegamorphic(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  // Get the receiver of the function from the stack into a1.
  __ lw(a1, MemOperand(sp, argc * kPointerSize));

  Label do_call, slow_call, slow_load, slow_reload_receiver;
  Label check_number_dictionary, check_string, lookup_monomorphic_cache;
  Label index_smi, index_string;

  // Check that the key is a smi.
  __ JumpIfNotSmi(a2, &check_string);
  __ bind(&index_smi);
  // Now the key is known to be a smi. This place is also jumped to from below
  // where a numeric string is converted to a smi.

  GenerateKeyedLoadReceiverCheck(
      masm, a1, a0, a3, Map::kHasIndexedInterceptor, &slow_call);

  GenerateFastArrayLoad(
      masm, a1, a2, t0, a3, a0, a1, &check_number_dictionary, &slow_load);
  __ IncrementCounter(&Counters::keyed_call_generic_smi_fast, 1, a0, a3);

  __ bind(&do_call);
  // receiver in a1 is not used after this point.
  // a2: key
  // a1: function

  GenerateFunctionTailCall(masm, argc, &slow_call, a0);

  __ bind(&check_number_dictionary);
  // a2: key
  // a3: elements map
  // t0: elements pointer
  // Check whether the elements is a number dictionary.
  __ LoadRoot(at, Heap::kHashTableMapRootIndex);
  __ Branch(&slow_load, ne, a3, Operand(at));
  __ sra(a0, a2, kSmiTagSize);
  // a0: untagged index
  GenerateNumberDictionaryLoad(masm, &slow_load, t0, a2, a1, a0, a3, t1);
  __ IncrementCounter(&Counters::keyed_call_generic_smi_dict, 1, a0, a3);
  __ jmp(&do_call);

  __ bind(&slow_load);
  // This branch is taken when calling KeyedCallIC_Miss is neither required
  // nor beneficial.
  __ IncrementCounter(&Counters::keyed_call_generic_slow_load, 1, a0, a3);
  __ EnterInternalFrame();
  __ push(a2);  // save the key
  __ Push(a1, a2);  // pass the receiver and the key
  __ CallRuntime(Runtime::kKeyedGetProperty, 2);
  __ pop(a2);  // restore the key
  __ LeaveInternalFrame();
  __ mov(a1, v0);
  __ jmp(&do_call);

  __ bind(&check_string);
  GenerateKeyStringCheck(masm, a2, a0, a3, &index_string, &slow_call);

  // The key is known to be a symbol.
  // If the receiver is a regular JS object with slow properties then do
  // a quick inline probe of the receiver's dictionary.
  // Otherwise do the monomorphic cache probe.
  GenerateKeyedLoadReceiverCheck(
      masm, a1, a0, a3, Map::kHasNamedInterceptor, &lookup_monomorphic_cache);

  __ lw(a0, FieldMemOperand(a1, JSObject::kPropertiesOffset));
  __ lw(a3, FieldMemOperand(a0, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHashTableMapRootIndex);
  __ Branch(&lookup_monomorphic_cache, ne, a3, Operand(at));

  GenerateDictionaryLoad(masm, &slow_load, a0, a2, a1, a3, t0);
  __ IncrementCounter(&Counters::keyed_call_generic_lookup_dict, 1, a0, a3);
  __ jmp(&do_call);

  __ bind(&lookup_monomorphic_cache);
  __ IncrementCounter(&Counters::keyed_call_generic_lookup_cache, 1, a0, a3);
  GenerateMonomorphicCacheProbe(masm, argc, Code::KEYED_CALL_IC);
  // Fall through on miss.

  __ bind(&slow_call);
  // This branch is taken if:
  // - the receiver requires boxing or access check,
  // - the key is neither smi nor symbol,
  // - the value loaded is not a function,
  // - there is hope that the runtime will create a monomorphic call stub
  //   that will get fetched next time.
  __ IncrementCounter(&Counters::keyed_call_generic_slow, 1, a0, a3);
  GenerateMiss(masm, argc);

  __ bind(&index_string);
  __ IndexFromHash(a3, a2);
  // Now jump to the place where smi keys are handled.
  __ jmp(&index_smi);
}


void KeyedCallIC::GenerateNormal(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  // Check if the name is a string.
  Label miss;
  __ JumpIfSmi(a2, &miss);
  __ IsObjectJSStringType(a2, a0, &miss);

  GenerateCallNormal(masm, argc);
  __ bind(&miss);
  GenerateMiss(masm, argc);
}


// Defined in ic.cc.
Object* LoadIC_Miss(Arguments args);

void LoadIC::GenerateMegamorphic(MacroAssembler* masm) {
  // a2    : name
  // ra    : return address
  // a0    : receiver
  // sp[0] : receiver

  // Probe the stub cache.
  Code::Flags flags = Code::ComputeFlags(Code::LOAD_IC,
                                         NOT_IN_LOOP,
                                         MONOMORPHIC);
  StubCache::GenerateProbe(masm, flags, a0, a2, a3, t0, t1);

  // Cache miss: Jump to runtime.
  GenerateMiss(masm);
}


void LoadIC::GenerateNormal(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- lr    : return address
  //  -- a0    : receiver
  //  -- sp[0] : receiver
  // -----------------------------------
  Label miss;

  GenerateStringDictionaryReceiverCheck(masm, a0, a1, a3, t0, &miss);

  // a1: elements
  GenerateDictionaryLoad(masm, &miss, a1, a2, v0, a3, t0);
  __ Ret();

  // Cache miss: Jump to runtime.
  __ bind(&miss);
  GenerateMiss(masm);
}


void LoadIC::GenerateMiss(MacroAssembler* masm) {
  // a2    : name
  // ra    : return address
  // a0    : receiver
  // sp[0] : receiver

  __ IncrementCounter(&Counters::keyed_load_miss, 1, a3, t0);

  __ mov(a3, a0);
  __ Push(a3, a2);

  // Perform tail call to the entry.
  ExternalReference ref = ExternalReference(IC_Utility(kLoadIC_Miss));
  __ TailCallExternalReference(ref, 2, 1);
}


// Returns the code marker, or the 0 if the code is not marked.
static inline int InlinedICSiteMarker(Address address,
                                      Address* inline_end_address) {
  if (V8::UseCrankshaft()) return false;

  // If the instruction after the call site is not the pseudo instruction nop(1)
  // then this is not related to an inlined in-object property load. The nop(1)
  // instruction is located just after the call to the IC in the deferred code
  // handling the miss in the inlined code. After the nop(1) instruction there
  // is a branch instruction for jumping back from the deferred code.
  Address address_after_call = address + Assembler::kCallTargetAddressOffset;
  Instr instr_after_call = Assembler::instr_at(address_after_call);

  int code_marker = MacroAssembler::GetCodeMarker(instr_after_call);

  // A negative result means the code is not marked.
  if (code_marker <= 0) return 0;

  Address address_after_nop = address_after_call + Assembler::kInstrSize;
  Instr instr_after_nop = Assembler::instr_at(address_after_nop);
  // There may be some reg-reg move and frame merging code to skip over before
  // the branch back from the DeferredReferenceGetKeyedValue code to the inlined
  // code.
  while (!(Assembler::IsBranch(instr_after_nop) ||
           Assembler::IsJr(instr_after_nop))) {
    address_after_nop += Assembler::kInstrSize;
    instr_after_nop = Assembler::instr_at(address_after_nop);
  }

  // Find the end of the inlined code for handling the load.
  int b_offset = 0;
  if (Assembler::IsBranch(instr_after_nop)) {
    b_offset = Assembler::GetBranchOffset(instr_after_nop) +
        Assembler::kPcLoadDelta;
  } else {
    b_offset = Assembler::GetJumpOffset(address_after_nop);
  }

  ASSERT(b_offset < 0);  // Jumping back from deferred code.
  *inline_end_address = address_after_nop + b_offset;

  return code_marker;
}


bool LoadIC::PatchInlinedLoad(Address address, Object* map, int offset) {
  if (V8::UseCrankshaft()) return false;

  // See CodeGenerator::EmitNamedLoad for
  // explanation of constants and instructions used here.

  // Find the end of the inlined code for handling the load if this is an
  // inlined IC call site.
  Address inline_end_address;
  if (InlinedICSiteMarker(address, &inline_end_address)
      != Assembler::PROPERTY_ACCESS_INLINED) {
    return false;
  }

  // Patch the offset of the property load instruction.
  // The immediate must be representable in 16 bits.
  ASSERT((JSObject::kMaxInstanceSize - JSObject::kHeaderSize) < (1 << 16));

  Address lw_property_instr_address =
          inline_end_address - Assembler::kInstrSize;
  ASSERT(Assembler::IsLw(Assembler::instr_at(lw_property_instr_address)));
  Instr lw_property_instr = Assembler::instr_at(lw_property_instr_address);

  lw_property_instr = Assembler::SetLwOffset(
      lw_property_instr, offset - kHeapObjectTag);
  Assembler::instr_at_put(lw_property_instr_address, lw_property_instr);

  // Indicate that code has changed.
  CPU::FlushICache(lw_property_instr_address, 1 * Assembler::kInstrSize);

  // Patch the map check.
  // For PROPERTY_ACCESS_INLINED, the load map instruction is generated
  // 5 instructions before the end of the inlined code.
  // See codegen-mips.cc CodeGenerator::EmitNamedLoad.
  int lw_map_offset = -5;
  Address li_map_instr_address = inline_end_address + lw_map_offset *
      Assembler::kInstrSize;
  Instr instr = Assembler::instr_at(li_map_instr_address);
  // This li instruction pair needs to be patched:
  // __ li(scratch2, Operand(Factory::null_value()), true);
  // scratch2 is not assembler temporary (at) register which is used for
  // load immediate in Branch macro. So, in that case, different code position
  // needs to be patched.
  if (!Assembler::IsLui(instr) ||
      (Assembler::IsLui(instr) &&
       Assembler::GetRt(instr) == (uint32_t)at.code())) {
    lw_map_offset = -9;
    li_map_instr_address = inline_end_address +
        lw_map_offset * Assembler::kInstrSize;
    ASSERT(Assembler::IsLui(Assembler::instr_at(li_map_instr_address)));
  }
#ifdef DEBUG
  Instr instr_lw =
      Assembler::instr_at(li_map_instr_address - 1 * Assembler::kInstrSize);
  Instr instr_branch =
      Assembler::instr_at(li_map_instr_address + 2 * Assembler::kInstrSize);
  ASSERT(Assembler::IsLw(instr_lw) && Assembler::IsBranch(instr_branch));
#endif

  Assembler::set_target_address_at(li_map_instr_address,
                                   reinterpret_cast<Address>(map));
  return true;
}


bool LoadIC::PatchInlinedContextualLoad(Address address,
                                        Object* map,
                                        Object* cell,
                                        bool is_dont_delete) {
  // Find the end of the inlined code for handling the contextual load if
  // this is inlined IC call site.
  Address inline_end_address;
  int marker = InlinedICSiteMarker(address, &inline_end_address);
  if (!((marker == Assembler::PROPERTY_ACCESS_INLINED_CONTEXT) ||
        (marker == Assembler::PROPERTY_ACCESS_INLINED_CONTEXT_DONT_DELETE))) {
    return false;
  }
  // On MIPS we don't rely on the is_dont_delete argument as the hint is already
  // embedded in the code marker.
  bool marker_is_dont_delete =
      marker == Assembler::PROPERTY_ACCESS_INLINED_CONTEXT_DONT_DELETE;

  // These are the offsets from the end of the inlined code.
  // See codegen-mips.cc CodeGenerator::EmitNamedLoad.
  int li_map_offset = marker_is_dont_delete ? -7: -11;
  int li_cell_offset = marker_is_dont_delete ? -3: -7;
  if (FLAG_debug_code && marker_is_dont_delete) {
    // Three extra instructions were generated to check for the_hole_value.
    li_map_offset -= 4;
    li_cell_offset -= 4;
  }
  Address li_map_instr_address =
      inline_end_address + li_map_offset * Assembler::kInstrSize;
  Address li_cell_instr_address =
      inline_end_address + li_cell_offset * Assembler::kInstrSize;
  Instr instr_li_map = Assembler::instr_at(li_map_instr_address);
  Instr instr_li_cell = Assembler::instr_at(li_cell_instr_address);
  // These li instruction pairs need to be patched:
  // 1. li(receiver, Operand(Factory::null_value()), true);
  // 2. li(scratch2, Operand(Factory::null_value()), true);
  // scratch2 is not assembler temporary (at) register which is used for
  // load immediate in Branch macro. So, in that case, different code position
  // needs to be patched.
  if ((Assembler::IsLui(instr_li_map) && Assembler::IsLui(instr_li_cell) &&
      (Assembler::GetRt(instr_li_map) == (uint32_t)at.code())) ||
      (Assembler::IsLui(instr_li_map) && Assembler::IsOri(instr_li_cell))) {
    li_map_offset = marker_is_dont_delete ? -11: -19;
    li_cell_offset = marker_is_dont_delete ? -3: -11;
    if (FLAG_debug_code && marker_is_dont_delete) {
      // Three extra instructions were generated to check for the_hole_value.
      li_map_offset -= 4;
      li_cell_offset -= 4;
    }
    li_map_instr_address =
        inline_end_address + li_map_offset * Assembler::kInstrSize;
    li_cell_instr_address =
        inline_end_address + li_cell_offset * Assembler::kInstrSize;
#ifdef DEBUG
    instr_li_map = Assembler::instr_at(li_map_instr_address);
    instr_li_cell = Assembler::instr_at(li_cell_instr_address);
    ASSERT(Assembler::IsLui(instr_li_map) && Assembler::IsLui(instr_li_cell));
#endif
  }
#ifdef DEBUG
  Instr instr_lw =
      Assembler::instr_at(li_cell_instr_address + 2 * Assembler::kInstrSize);
  Instr instr_branch =
      Assembler::instr_at(li_map_instr_address + 2 * Assembler::kInstrSize);
  ASSERT(Assembler::IsLw(instr_lw) && Assembler::IsBranch(instr_branch));
#endif

  // Patch the map check.
  Assembler::set_target_address_at(li_map_instr_address,
                                   reinterpret_cast<Address>(map));
  // Patch the cell address.
  Assembler::set_target_address_at(li_cell_instr_address,
                                   reinterpret_cast<Address>(cell));

  return true;
}


bool StoreIC::PatchInlinedStore(Address address, Object* map, int offset) {
  if (V8::UseCrankshaft()) return false;

  // Find the end of the inlined code for the store if there is an
  // inlined version of the store.
  Address inline_end_address;
  if (InlinedICSiteMarker(address, &inline_end_address)
      != Assembler::PROPERTY_ACCESS_INLINED) {
    return false;
  }

  // Compute the address of the map load instruction.
  bool after_trampoline_emission = false;
  Address li_map_instr_address =
      inline_end_address -
      (CodeGenerator::GetInlinedNamedStoreInstructionsAfterPatch(false) *
       Assembler::kInstrSize);
  Instr instr = Assembler::instr_at(li_map_instr_address);
  // This li instruction pair needs to be patched:
  // __ li(scratch0, Operand(Factory::null_value()), true);
  // scratch0 is not assembler temporary (at) register which is used for
  // load immediate in Branch macro. So, in that case, different code position
  // needs to be patched.
  if (!Assembler::IsLui(instr) ||
      (Assembler::IsLui(instr) &&
       Assembler::GetRt(instr) == (uint32_t)at.code())) {
    li_map_instr_address =
      inline_end_address -
      (CodeGenerator::GetInlinedNamedStoreInstructionsAfterPatch(true) *
       Assembler::kInstrSize);
    instr = Assembler::instr_at(li_map_instr_address);
    ASSERT(Assembler::IsLui(instr));
    after_trampoline_emission = true;
  }
#ifdef DEBUG
  Instr instr_lw =
      Assembler::instr_at(li_map_instr_address - 1 * Assembler::kInstrSize);
  Instr instr_branch =
      Assembler::instr_at(li_map_instr_address + 2 * Assembler::kInstrSize);
  ASSERT(Assembler::IsLw(instr_lw) && Assembler::IsBranch(instr_branch));
#endif

  // Update the offsets if initializing the inlined store. No reason
  // to update the offsets when clearing the inlined version because
  // it will bail out in the map check.
  if (map != Heap::null_value()) {
    // Patch the offset in the actual store instruction.
    // Magic number 4 is instruction count before the 'sw' instr we patch.
    // These are: li(liu & ori), and Branch (bne & nop).
    Address sw_property_instr_address =
        li_map_instr_address + 4 * Assembler::kInstrSize;
    if (after_trampoline_emission) {
      sw_property_instr_address += 4 * Assembler::kInstrSize;
    }
    Instr sw_property_instr = Assembler::instr_at(sw_property_instr_address);
    ASSERT(Assembler::IsSw(sw_property_instr));
    sw_property_instr = Assembler::SetSwOffset(
        sw_property_instr, offset - kHeapObjectTag);
    Assembler::instr_at_put(sw_property_instr_address, sw_property_instr);

    // Patch the offset in the add instruction that is part of the
    // (in-lined) write barrier. The Add comes 1 instrunction after the
    // 'sw' instruction, hence the magic-number 1 below.
    Address add_offset_instr_address =
        sw_property_instr_address + 1 * Assembler::kInstrSize;
    Instr add_offset_instr = Assembler::instr_at(add_offset_instr_address);
    ASSERT(Assembler::IsAddImmediate(add_offset_instr));
    add_offset_instr = Assembler::SetAddImmediateOffset(
        add_offset_instr, offset - kHeapObjectTag);
    Assembler::instr_at_put(add_offset_instr_address, add_offset_instr);

    // Indicate that code has changed.
    // Magic number 2, covers updated (consecutive) 'sw', and 'add' instrs.
    CPU::FlushICache(sw_property_instr_address, 2 * Assembler::kInstrSize);
  }

  // Patch the map check.
  // This does its own i-cache-flush.
  Assembler::set_target_address_at(li_map_instr_address,
                                   reinterpret_cast<Address>(map));

  return true;
}


bool KeyedLoadIC::PatchInlinedLoad(Address address, Object* map) {
  if (V8::UseCrankshaft()) return false;

  Address inline_end_address;
  if (InlinedICSiteMarker(address, &inline_end_address)
      != Assembler::PROPERTY_ACCESS_INLINED) {
    return false;
  }

  // Patch the map check.
  // This code patches CodeGenerator::EmitKeyedLoad(), at the
  // li(scratch2, Operand(Factory::null_value()), true); which at
  // present is 24 instructions from the end of the routine.
  Address li_map_instr_address = 0;
  li_map_instr_address =
      inline_end_address -
      (CodeGenerator::GetInlinedKeyedLoadInstructionsAfterPatch(false) *
      Assembler::kInstrSize);
  Instr instr = Assembler::instr_at(li_map_instr_address);
  if (!Assembler::IsLui(instr)) {
    li_map_instr_address =
      inline_end_address -
      (CodeGenerator::GetInlinedKeyedLoadInstructionsAfterPatch(true) *
      Assembler::kInstrSize);
    instr = Assembler::instr_at(li_map_instr_address);
    ASSERT(Assembler::IsLui(instr));
  }
#ifdef DEBUG
  Instr instr_branch =
      Assembler::instr_at(li_map_instr_address + 2 * Assembler::kInstrSize);
  ASSERT(Assembler::IsBranch(instr_branch));
#endif
  Assembler::set_target_address_at(li_map_instr_address,
                                   reinterpret_cast<Address>(map));
  return true;
}


bool KeyedStoreIC::PatchInlinedStore(Address address, Object* map) {
  if (V8::UseCrankshaft()) return false;

  // Find the end of the inlined code for handling the store if this is an
  // inlined IC call site.
  Address inline_end_address;
  if (InlinedICSiteMarker(address, &inline_end_address)
      != Assembler::PROPERTY_ACCESS_INLINED) {
    return false;
  }

  // Patch the map check.
  // This code patches CodeGenerator::EmitKeyedStore(), at the
  // __ li(t1, Operand(Factory::fixed_array_map()), true);
  // which is 'kInlinedKeyedStoreInstructionsAfterPatch'
  // instructions from the end of the routine.
  Address li_map_instr_address =
      inline_end_address -
      (CodeGenerator::GetInlinedKeyedStoreInstructionsAfterPatch(false) *
      Assembler::kInstrSize);
  Instr instr = Assembler::instr_at(li_map_instr_address);
  if (!Assembler::IsLui(instr)) {
    Address li_map_instr_address =
      inline_end_address -
      (CodeGenerator::GetInlinedKeyedStoreInstructionsAfterPatch(true) *
      Assembler::kInstrSize);
    instr = Assembler::instr_at(li_map_instr_address);
    ASSERT(Assembler::IsLui(instr));
  }
#ifdef DEBUG
  Instr instr_lw =
      Assembler::instr_at(li_map_instr_address - 1 * Assembler::kInstrSize);
  Instr instr_branch =
      Assembler::instr_at(li_map_instr_address + 2 * Assembler::kInstrSize);
  ASSERT(Assembler::IsLw(instr_lw) && Assembler::IsBranch(instr_branch));
#endif
  Assembler::set_target_address_at(li_map_instr_address,
                                   reinterpret_cast<Address>(map));
  return true;
}


Object* KeyedLoadIC_Miss(Arguments args);


void KeyedLoadIC::GenerateMiss(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- a1     : receiver
  // -----------------------------------

  __ IncrementCounter(&Counters::keyed_load_miss, 1, a3, t0);

  __ Push(a1, a0);

  ExternalReference ref = ExternalReference(IC_Utility(kKeyedLoadIC_Miss));
  __ TailCallExternalReference(ref, 2, 1);
}


void KeyedLoadIC::GenerateRuntimeGetProperty(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- a1     : receiver
  // -----------------------------------

  __ Push(a1, a0);

  __ TailCallRuntime(Runtime::kKeyedGetProperty, 2, 1);
}


void KeyedLoadIC::GenerateGeneric(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- a1     : receiver
  // -----------------------------------
  Label slow, check_string, index_smi, index_string, property_array_property;
  Label probe_dictionary, check_number_dictionary;

  Register key = a0;
  Register receiver = a1;

  // Check that the key is a smi.
  __ JumpIfNotSmi(key, &check_string);
  __ bind(&index_smi);
  // Now the key is known to be a smi. This place is also jumped to from below
  // where a numeric string is converted to a smi.

  GenerateKeyedLoadReceiverCheck(
      masm, receiver, a2, a3, Map::kHasIndexedInterceptor, &slow);

  // Check the "has fast elements" bit in the receiver's map which is
  // now in a2.
  __ lbu(a3, FieldMemOperand(a2, Map::kBitField2Offset));
  __ And(at, a3, Operand(1 << Map::kHasFastElements));
  __ Branch(&check_number_dictionary, eq, at, Operand(zero_reg));

  GenerateFastArrayLoad(
      masm, receiver, key, t0, a3, a2, v0, NULL, &slow);

  __ IncrementCounter(&Counters::keyed_load_generic_smi, 1, a2, a3);
  __ Ret();

  __ bind(&check_number_dictionary);
  __ lw(t0, FieldMemOperand(receiver, JSObject::kElementsOffset));
  __ lw(a3, FieldMemOperand(t0, JSObject::kMapOffset));

  // Check whether the elements is a number dictionary.
  // a0: key
  // a3: elements map
  // t0: elements
  __ LoadRoot(at, Heap::kHashTableMapRootIndex);
  __ Branch(&slow, ne, a3, Operand(at));
  __ sra(a2, a0, kSmiTagSize);
  GenerateNumberDictionaryLoad(masm, &slow, t0, a0, v0, a2, a3, t1);
  __ Ret();

  // Slow case, key and receiver still in a0 and a1.
  __ bind(&slow);
  __ IncrementCounter(&Counters::keyed_load_generic_slow, 1, a2, a3);
  GenerateRuntimeGetProperty(masm);

  __ bind(&check_string);
  GenerateKeyStringCheck(masm, key, a2, a3, &index_string, &slow);

  GenerateKeyedLoadReceiverCheck(
       masm, receiver, a2, a3, Map::kHasIndexedInterceptor, &slow);


  // If the receiver is a fast-case object, check the keyed lookup
  // cache. Otherwise probe the dictionary.
  __ lw(a3, FieldMemOperand(a1, JSObject::kPropertiesOffset));
  __ lw(t0, FieldMemOperand(a3, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHashTableMapRootIndex);
  __ Branch(&probe_dictionary, eq, t0, Operand(at));

  // Load the map of the receiver, compute the keyed lookup cache hash
  // based on 32 bits of the map pointer and the string hash.
  __ lw(a2, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ sra(a3, a2, KeyedLookupCache::kMapHashShift);
  __ lw(t0, FieldMemOperand(a0, String::kHashFieldOffset));
  __ sra(at, t0, String::kHashShift);
  __ xor_(a3, a3, at);
  __ And(a3, a3, Operand(KeyedLookupCache::kCapacityMask));

  // Load the key (consisting of map and symbol) from the cache and
  // check for match.
  ExternalReference cache_keys = ExternalReference::keyed_lookup_cache_keys();
  __ li(t0, Operand(cache_keys));
  __ sll(at, a3, kPointerSizeLog2 + 1);
  __ addu(t0, t0, at);
  __ lw(t1, MemOperand(t0));  // Move t0 to symbol.
  __ Addu(t0, t0, Operand(kPointerSize));
  __ Branch(&slow, ne, a2, Operand(t1));
  __ lw(t1, MemOperand(t0));
  __ Branch(&slow, ne, a0, Operand(t1));

  // Get field offset.
  // a0     : key
  // a1     : receiver
  // a2     : receiver's map
  // a3     : lookup cache index
  ExternalReference cache_field_offsets
      = ExternalReference::keyed_lookup_cache_field_offsets();
  __ li(t0, Operand(cache_field_offsets));
  __ sll(at, a3, kPointerSizeLog2);
  __ addu(at, t0, at);
  __ lw(t1, MemOperand(at));
  __ lbu(t2, FieldMemOperand(a2, Map::kInObjectPropertiesOffset));
  __ Subu(t1, t1, t2);
  __ Branch(&property_array_property, ge, t1, Operand(zero_reg));

  // Load in-object property.
  __ lbu(t2, FieldMemOperand(a2, Map::kInstanceSizeOffset));
  __ addu(t2, t2, t1);  // Index from start of object.
  __ Subu(a1, a1, Operand(kHeapObjectTag));  // Remove the heap tag.
  __ sll(at, t2, kPointerSizeLog2);
  __ addu(at, a1, at);
  __ lw(v0, MemOperand(at));
  __ IncrementCounter(&Counters::keyed_load_generic_lookup_cache, 1, a2, a3);
  __ Ret();

  // Load property array property.
  __ bind(&property_array_property);
  __ lw(a1, FieldMemOperand(a1, JSObject::kPropertiesOffset));
  __ Addu(a1, a1, FixedArray::kHeaderSize - kHeapObjectTag);
  __ sll(t0, t1, kPointerSizeLog2);
  __ Addu(t0, t0, a1);
  __ lw(v0, MemOperand(t0));
  __ IncrementCounter(&Counters::keyed_load_generic_lookup_cache, 1, a2, a3);
  __ Ret();


  // Do a quick inline probe of the receiver's dictionary, if it
  // exists.
  __ bind(&probe_dictionary);
  // a1: receiver
  // a0: key
  // a3: elements
  __ lw(a2, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ lbu(a2, FieldMemOperand(a2, Map::kInstanceTypeOffset));
  GenerateGlobalInstanceTypeCheck(masm, a2, &slow);
  // Load the property to v0.
  GenerateDictionaryLoad(masm, &slow, a3, a0, v0, a2, t0);
  __ IncrementCounter(&Counters::keyed_load_generic_symbol, 1, a2, a3);
  __ Ret();

  __ bind(&index_string);
  __ IndexFromHash(a3, key);
  // Now jump to the place where smi keys are handled.
  __ Branch(&index_smi);
}


void KeyedLoadIC::GenerateString(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key (index)
  //  -- a1     : receiver
  // -----------------------------------
  Label miss;

  Register receiver = a1;
  Register index = a0;
  Register scratch1 = a2;
  Register scratch2 = a3;
  Register result = v0;

  StringCharAtGenerator char_at_generator(receiver,
                                          index,
                                          scratch1,
                                          scratch2,
                                          result,
                                          &miss,  // When not a string.
                                          &miss,  // When not a number.
                                          &miss,  // When index out of range.
                                          STRING_INDEX_IS_ARRAY_INDEX);
  char_at_generator.GenerateFast(masm);
  __ Ret();

  StubRuntimeCallHelper call_helper;
  char_at_generator.GenerateSlow(masm, call_helper);

  __ bind(&miss);
  GenerateMiss(masm);
}


void KeyedStoreIC::GenerateRuntimeSetProperty(MacroAssembler* masm,
                                              StrictModeFlag strict_mode) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- a1     : key
  //  -- a2     : receiver
  //  -- ra     : return address
  // -----------------------------------

  // Push receiver, key and value for runtime call.
  __ Push(a2, a1, a0);
  __ li(a1, Operand(Smi::FromInt(NONE)));          // PropertyAttributes
  __ li(a0, Operand(Smi::FromInt(strict_mode)));   // Strict mode.
  __ Push(a1, a0);

  __ TailCallRuntime(Runtime::kSetProperty, 5, 1);
}


void KeyedStoreIC::GenerateGeneric(MacroAssembler* masm,
                                   StrictModeFlag strict_mode) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- a1     : key
  //  -- a2     : receiver
  //  -- ra     : return address
  // -----------------------------------

  Label slow, fast, array, extra, exit;

  // Register usage.
  Register value = a0;
  Register key = a1;
  Register receiver = a2;
  Register elements = a3;  // Elements array of the receiver.
  // t0 is used as ip in the arm version.
  // t3-t4 are used as temporaries.

  // Check that the key is a smi.
  __ JumpIfNotSmi(key, &slow);
  // Check that the object isn't a smi.
  __ JumpIfSmi(receiver, &slow);

  // Get the map of the object.
  __ lw(t3, FieldMemOperand(receiver, HeapObject::kMapOffset));
  // Check that the receiver does not require access checks.  We need
  // to do this because this generic stub does not perform map checks.
  __ lbu(t0, FieldMemOperand(t3, Map::kBitFieldOffset));
  __ And(t0, t0, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&slow, ne, t0, Operand(zero_reg));
  // Check if the object is a JS array or not.
  __ lbu(t3, FieldMemOperand(t3, Map::kInstanceTypeOffset));

  __ Branch(&array, eq, t3, Operand(JS_ARRAY_TYPE));
  // Check that the object is some kind of JS object.
  __ Branch(&slow, lt, t3, Operand(FIRST_JS_OBJECT_TYPE));

  // Object case: Check key against length in the elements array.
  __ lw(elements, FieldMemOperand(receiver, JSObject::kElementsOffset));
  // Check that the object is in fast mode and writable.
  __ lw(t3, FieldMemOperand(elements, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::kFixedArrayMapRootIndex);
  __ Branch(&slow, ne, t3, Operand(t0));
  // Check array bounds. Both the key and the length of FixedArray are smis.
  __ lw(t0, FieldMemOperand(elements, FixedArray::kLengthOffset));
  __ Branch(&fast, lo, key, Operand(t0));
  // Fall thru to slow if un-tagged index >= length.

  // Slow case, handle jump to runtime.
  __ bind(&slow);

  // Entry registers are intact.
  // a0: value.
  // a1: key.
  // a2: receiver.

  GenerateRuntimeSetProperty(masm, strict_mode);

  // Extra capacity case: Check if there is extra capacity to
  // perform the store and update the length. Used for adding one
  // element to the array by writing to array[array.length].

  __ bind(&extra);
  // Only support writing to array[array.length].
  __ Branch(&slow, ne, key, Operand(t0));
  // Check for room in the elements backing store.
  // Both the key and the length of FixedArray are smis.
  __ lw(t0, FieldMemOperand(elements, FixedArray::kLengthOffset));
  __ Branch(&slow, hs, key, Operand(t0));
  // Calculate key + 1 as smi.
  ASSERT_EQ(0, kSmiTag);
  __ Addu(t3, key, Operand(Smi::FromInt(1)));
  __ sw(t3, FieldMemOperand(receiver, JSArray::kLengthOffset));
  __ Branch(&fast);


  // Array case: Get the length and the elements array from the JS
  // array. Check that the array is in fast mode (and writable); if it
  // is the length is always a smi.

  __ bind(&array);
  __ lw(elements, FieldMemOperand(receiver, JSObject::kElementsOffset));
  __ lw(t3, FieldMemOperand(elements, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::kFixedArrayMapRootIndex);
  __ Branch(&slow, ne, t3, Operand(t0));

  // Check the key against the length in the array.
  __ lw(t0, FieldMemOperand(receiver, JSArray::kLengthOffset));
  __ Branch(&extra, hs, key, Operand(t0));
  // Fall through to fast case.

  __ bind(&fast);
  // Fast case, store the value to the elements backing store.
  __ Addu(t4, elements, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t1, key, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(t4, t4, Operand(t1));
  __ sw(value, MemOperand(t4));
  // Skip write barrier if the written value is a smi.
  __ JumpIfSmi(value, &exit);

  // Update write barrier for the elements array address.
  __ Subu(t3, t4, Operand(elements));

  __ RecordWrite(elements, Operand(t3), t4, t5);
  __ bind(&exit);

  __ mov(v0, a0);  // Return the value written.
  __ Ret();
}


void KeyedLoadIC::GenerateIndexedInterceptor(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- a1     : receiver
  // -----------------------------------
  Label slow;

  // Check that the receiver isn't a smi.
  __ JumpIfSmi(a1, &slow);

  // Check that the key is an array index, that is Uint32.
  __ And(t0, a0, Operand(kSmiTagMask | kSmiSignMask));
  __ Branch(&slow, ne, t0, Operand(zero_reg));

  // Get the map of the receiver.
  __ lw(a2, FieldMemOperand(a1, HeapObject::kMapOffset));

  // Check that it has indexed interceptor and access checks
  // are not enabled for this object.
  __ lbu(a3, FieldMemOperand(a2, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(kSlowCaseBitFieldMask));
  __ Branch(&slow, ne, a3, Operand(1 << Map::kHasIndexedInterceptor));
  // Everything is fine, call runtime.
  __ Push(a1, a0);  // Receiver, key.

  // Perform tail call to the entry.
  __ TailCallExternalReference(ExternalReference(
       IC_Utility(kKeyedLoadPropertyWithInterceptor)), 2, 1);

  __ bind(&slow);
  GenerateMiss(masm);
}


void KeyedStoreIC::GenerateMiss(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- a1     : key
  //  -- a2     : receiver
  //  -- ra     : return address
  // -----------------------------------

  // Push receiver, key and value for runtime call.
  // We can't use MultiPush as the order of the registers is important.
  __ Push(a2, a1, a0);

  ExternalReference ref = ExternalReference(IC_Utility(kKeyedStoreIC_Miss));
  __ TailCallExternalReference(ref, 3, 1);
}


void StoreIC::GenerateMegamorphic(MacroAssembler* masm,
                                  StrictModeFlag strict_mode) {
  // a0    : value
  // a1    : receiver
  // a2    : name
  // ra    : return address

  // Get the receiver from the stack and probe the stub cache.
  Code::Flags flags = Code::ComputeFlags(Code::STORE_IC,
                                         NOT_IN_LOOP,
                                         MONOMORPHIC,
                                         strict_mode);
  StubCache::GenerateProbe(masm, flags, a1, a2, a3, t0, t1);

  // Cache miss: Jump to runtime.
  GenerateMiss(masm);
}


void StoreIC::GenerateMiss(MacroAssembler* masm) {
  // a0    : value
  // a1    : receiver
  // a2    : name
  // ra    : return address

  __ Push(a1, a2, a0);
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
  __ JumpIfSmi(receiver, &miss);

  // Check that the object is a JS array.
  __ GetObjectType(receiver, scratch, scratch);
  __ Branch(&miss, ne, scratch, Operand(JS_ARRAY_TYPE));

  // Check that elements are FixedArray.
  // We rely on StoreIC_ArrayLength below to deal with all types of
  // fast elements (including COW).
  __ lw(scratch, FieldMemOperand(receiver, JSArray::kElementsOffset));
  __ GetObjectType(scratch, scratch, scratch);
  __ Branch(&miss, ne, scratch, Operand(FIXED_ARRAY_TYPE));

  // Check that value is a smi.
  __ JumpIfNotSmi(value, &miss);

  // Prepare tail call to StoreIC_ArrayLength.
  __ Push(receiver, value);

  ExternalReference ref = ExternalReference(IC_Utility(kStoreIC_ArrayLength));
  __ TailCallExternalReference(ref, 2, 1);

  __ bind(&miss);

  GenerateMiss(masm);
}


void StoreIC::GenerateNormal(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- a0    : value
  //  -- a1    : receiver
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------
  Label miss;

  GenerateStringDictionaryReceiverCheck(masm, a1, a3, t0, t1, &miss);

  GenerateDictionaryStore(masm, &miss, a3, a2, a0, t0, t1);
  __ IncrementCounter(&Counters::store_normal_hit, 1, t0, t1);
  __ Ret();

  __ bind(&miss);
  __ IncrementCounter(&Counters::store_normal_miss, 1, t0, t1);
  GenerateMiss(masm);
}


void StoreIC::GenerateGlobalProxy(MacroAssembler* masm,
                                  StrictModeFlag strict_mode) {
  // ----------- S t a t e -------------
  //  -- a0    : value
  //  -- a1    : receiver
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  __ Push(a1, a2, a0);

  __ li(a1, Operand(Smi::FromInt(NONE)));  // PropertyAttributes
  __ li(a0, Operand(Smi::FromInt(strict_mode)));
  __ Push(a1, a0);

  // Do tail-call to runtime routine.
  __ TailCallRuntime(Runtime::kSetProperty, 5, 1);
}


#undef __


Condition CompareIC::ComputeCondition(Token::Value op) {
  switch (op) {
    case Token::EQ_STRICT:
    case Token::EQ:
      return eq;
    case Token::LT:
      return lt;
    case Token::GT:
      // Reverse left and right operands to obtain ECMA-262 conversion order.
      return lt;
    case Token::LTE:
      // Reverse left and right operands to obtain ECMA-262 conversion order.
      return ge;
    case Token::GTE:
      return ge;
    default:
      UNREACHABLE();
      return kNoCondition;
  }
}


void CompareIC::UpdateCaches(Handle<Object> x, Handle<Object> y) {
  HandleScope scope;
  Handle<Code> rewritten;
  State previous_state = GetState();
  State state = TargetState(previous_state, false, x, y);
  if (state == GENERIC) {
    CompareStub stub(GetCondition(), strict(), NO_COMPARE_FLAGS, a1, a0);
    rewritten = stub.GetCode();
  } else {
    ICCompareStub stub(op_, state);
    rewritten = stub.GetCode();
  }
  set_target(*rewritten);

#ifdef DEBUG
  if (FLAG_trace_ic) {
    PrintF("[CompareIC (%s->%s)#%s]\n",
           GetStateName(previous_state),
           GetStateName(state),
           Token::Name(op_));
  }
#endif
}


void PatchInlinedSmiCode(Address address) {
  // Currently there is no smi inlining in the MIPS full code generator.
}


} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_MIPS
