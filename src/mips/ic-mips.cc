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
// receiver: Receiver. It is not clobbered if a jump to the miss label is
//           done
// name:     Property name. It is not clobbered if a jump to the miss label is
//           done
// result:   Register for the result. It is only updated if a jump to the miss
//           label is not done. Can be the same as receiver or name clobbering
//           one of these in the case of not jumping to the miss label.
// The three scratch registers need to be different from the receiver, name and
// result.
static void GenerateDictionaryLoad(MacroAssembler* masm,
                                   Label* miss,
                                   Register receiver,
                                   Register name,
                                   Register result,
                                   Register scratch1,
                                   Register scratch2,
                                   Register scratch3,
                                   DictionaryCheck check_dictionary) {
  // Main use of the scratch registers.
  // scratch1: Used to hold the property dictionary.
  // scratch2: Used as temporary and to hold the capacity of the property
  //           dictionary.
  // scratch3: Used as temporary.

  Label done;

  // Check for the absence of an interceptor.
  // Load the map into scratch1.
  __ lw(scratch1, FieldMemOperand(receiver, JSObject::kMapOffset));

  // Bail out if the receiver has a named interceptor.
  __ lbu(scratch2, FieldMemOperand(scratch1, Map::kBitFieldOffset));
  __ And(at, scratch2, Operand(1 << Map::kHasNamedInterceptor));
  __ Branch(miss, ne, at, Operand(zero_reg));

  // Bail out if we have a JS global proxy object.
  __ lbu(scratch2, FieldMemOperand(scratch1, Map::kInstanceTypeOffset));
  __ Branch(miss, eq, scratch2, Operand(JS_GLOBAL_PROXY_TYPE));

  // Possible work-around for http://crbug.com/16276.
  // See also: http://codereview.chromium.org/155418.
  __ Branch(miss, eq, scratch2, Operand(JS_GLOBAL_OBJECT_TYPE));
  __ Branch(miss, eq, scratch2, Operand(JS_BUILTINS_OBJECT_TYPE));

  // Load the properties array.
  __ lw(scratch1, FieldMemOperand(receiver, JSObject::kPropertiesOffset));

  // Check that the properties array is a dictionary.
  if (check_dictionary == CHECK_DICTIONARY) {
    __ lw(scratch2, FieldMemOperand(scratch1, HeapObject::kMapOffset));
    __ LoadRoot(at, Heap::kHashTableMapRootIndex);
    __ Branch(miss, ne, scratch2, Operand(at));
  }

  // Compute the capacity mask.
  const int kCapacityOffset = StringDictionary::kHeaderSize +
      StringDictionary::kCapacityIndex * kPointerSize;
  __ lw(scratch2, FieldMemOperand(scratch1, kCapacityOffset));
  __ sra(scratch2, scratch2, kSmiTagSize);  // Convert smi to int.
  __ Subu(scratch2, scratch2, Operand(1));

  const int kElementsStartOffset = StringDictionary::kHeaderSize +
      StringDictionary::kElementsStartIndex * kPointerSize;

  // Generate an unrolled loop that performs a few probes before
  // giving up. Measurements done on Gmail indicate that 2 probes
  // cover ~93% of loads from dictionaries.
  static const int kProbes = 4;
  for (int i = 0; i < kProbes; i++) {
    // Compute the masked index: (hash + i + i * i) & mask.
    __ lw(scratch3, FieldMemOperand(name, String::kHashFieldOffset));
    if (i > 0) {
      // Add the probe offset (i + i * i) left shifted to avoid right shifting
      // the hash in a separate instruction. The value hash + i + i * i is right
      // shifted in the following and instruction.
      ASSERT(StringDictionary::GetProbeOffset(i) <
             1 << (32 - String::kHashFieldOffset));
      __ Addu(scratch3, scratch3, Operand(
          StringDictionary::GetProbeOffset(i) << String::kHashShift));
    }

    __ srl(scratch3, scratch3, String::kHashShift);
    __ And(scratch3, scratch2, scratch3);

    // Scale the index by multiplying by the element size.
    ASSERT(StringDictionary::kEntrySize == 3);
    __ sll(at, scratch3, 1);  // at = scratch3 * 2.
    __ addu(scratch3, scratch3, at);  // scratch3 = scratch3 * 3.

    // Check if the key is identical to the name.
    __ sll(at, scratch3, 2);
    __ addu(scratch3, scratch1, at);
    __ lw(at, FieldMemOperand(scratch3, kElementsStartOffset));
    if (i != kProbes - 1) {
      __ Branch(&done, eq, name, Operand(at));
    } else {
      __ Branch(miss, ne, name, Operand(at));
    }
  }

  // Check that the value is a normal property.
  __ bind(&done);  // scratch3 == scratch1 + 4 * index.
  __ lw(scratch2,
        FieldMemOperand(scratch3, kElementsStartOffset + 2 * kPointerSize));
  __ And(scratch2,
         scratch2,
         Operand(PropertyDetails::TypeField::mask() << kSmiTagSize));
  __ Branch(miss, ne, scratch2, Operand(zero_reg));

  // Get the value at the masked, scaled index and return.
  __ lw(result,
        FieldMemOperand(scratch3, kElementsStartOffset + 1 * kPointerSize));
}


static void GenerateNumberDictionaryLoad(MacroAssembler* masm,
                                         Label* miss,
                                         Register elements,
                                         Register key,
                                         Register reg0,
                                         Register reg1,
                                         Register reg2) {
  // Register use:
  //
  // elements - holds the slow-case elements of the receiver and is unchanged.
  //
  // key      - holds the smi key on entry and is unchanged if a branch is
  //            performed to the miss label.
  //            Holds the result on exit if the load succeeded.
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
  __ lw(key, FieldMemOperand(reg2, kValueOffset));
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


void LoadIC::GenerateStringLength(MacroAssembler* masm) {
  // a2    : name
  // lr    : return address
  // a0    : receiver
  // sp[0] : receiver
  Label miss;

  StubCompiler::GenerateLoadStringLength(masm, a0, a1, a3, &miss);
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


// Defined in ic.cc.
Object* CallIC_Miss(Arguments args);

void CallIC::GenerateMegamorphic(MacroAssembler* masm, int argc) {
  // a2    : name
  // lr: return address

  Label number, non_number, non_string, boolean, probe, miss;

  // Get the receiver of the function from the stack into a1.
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


static void GenerateNormalHelper(MacroAssembler* masm,
                                 int argc,
                                 bool is_global_object,
                                 Label* miss,
                                 Register scratch) {
  // Search dictionary - put result in register a1.
  GenerateDictionaryLoad(masm, miss, a1, a2, a1, a0, a3, t0, CHECK_DICTIONARY);

  // Check that the value isn't a smi.
  __ BranchOnSmi(a1, miss);

  // Check that the value is a JSFunction.
  __ GetObjectType(a1, scratch, scratch);
  __ Branch(miss, ne, scratch, Operand(JS_FUNCTION_TYPE));

  // Patch the receiver with the global proxy if necessary.
  if (is_global_object) {
    __ lw(a0, MemOperand(sp, argc * kPointerSize));
    __ lw(a0, FieldMemOperand(a0, GlobalObject::kGlobalReceiverOffset));
    __ sw(a0, MemOperand(sp, argc * kPointerSize));
  }

  // Invoke the function.
  ParameterCount actual(argc);
  __ InvokeFunction(a1, actual, JUMP_FUNCTION);
}


void CallIC::GenerateNormal(MacroAssembler* masm, int argc) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------
  Label miss, global_object, non_global_object;

  // Get the receiver of the function from the stack into a1.
  __ lw(a1, MemOperand(sp, argc * kPointerSize));

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(a1, &miss);

  // Check that the receiver is a valid JS object.  Put the map in a3.
  __ GetObjectType(a1, a3, a0);
  __ Branch(&miss, lt, a0, Operand(FIRST_JS_OBJECT_TYPE));

  // If this assert fails, we have to check upper bound too.
  ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);

  // Check for access to global object.
  __ Branch(&global_object, eq, a0, Operand(JS_GLOBAL_OBJECT_TYPE));
  __ Branch(&non_global_object, ne, a0, Operand(JS_BUILTINS_OBJECT_TYPE));

  // Accessing global object: Load and invoke.
  __ bind(&global_object);
  // Check that the global object does not require access checks.
  __ lbu(a3, FieldMemOperand(a3, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&miss, ne, a3, Operand(zero_reg));
  GenerateNormalHelper(masm, argc, true, &miss, t0);

  // Accessing non-global object: Check for access to global proxy.
  Label global_proxy, invoke;
  __ bind(&non_global_object);
  __ Branch(&global_proxy, eq, a0, Operand(JS_GLOBAL_PROXY_TYPE));
  // Check that the non-global, non-global-proxy object does not
  // require access checks.
  __ lbu(a3, FieldMemOperand(a3, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&miss, ne, a3, Operand(zero_reg));
  __ bind(&invoke);
  GenerateNormalHelper(masm, argc, false, &miss, t0);

  // Global object access: Check access rights.
  __ bind(&global_proxy);
  __ CheckAccessGlobalProxy(a1, a0, &miss);
  __ Branch(&invoke, al);

  // Cache miss: Jump to runtime.
  __ bind(&miss);
  GenerateMiss(masm, argc);
}

void CallIC::GenerateMiss(MacroAssembler* masm, int argc) {
    // Registers:
    // a2: name
    // ra: return address

  // Get the receiver of the function from the stack.
  __ lw(a3, MemOperand(sp, argc*kPointerSize));

  __ EnterInternalFrame();

  // Push the receiver and the name of the function.
  __ Push(a3, a2);

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
  // a0    : receiver
  // sp[0] : receiver

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
  //  -- a0    : receiver
  //  -- sp[0] : receiver
  // -----------------------------------
  Label miss, probe, global;

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
  GenerateDictionaryLoad(masm, &miss, a0, a2, v0, a1, a3, t0, CHECK_DICTIONARY);
  __ Ret();

  // Global object access: Check access rights.
  __ bind(&global);
  __ CheckAccessGlobalProxy(a0, a1, &miss);
  __ Branch(&probe);

  // Cache miss: Jump to runtime.
  __ bind(&miss);
  GenerateMiss(masm);
}


void LoadIC::GenerateMiss(MacroAssembler* masm) {
  // a2    : name
  // ra    : return address
  // a0    : receiver
  // sp[0] : receiver

  __ mov(a3, a0);
  __ Push(a3, a2);

  // Perform tail call to the entry.
  ExternalReference ref = ExternalReference(IC_Utility(kLoadIC_Miss));
  __ TailCallExternalReference(ref, 2, 1);
}


static inline bool IsInlinedICSite(Address address,
                                   Address* inline_end_address) {
  // If the instruction after the call site is not the pseudo instruction nop(1)
  // then this is not related to an inlined in-object property load. The nop(1)
  // instruction is located just after the call to the IC in the deferred code
  // handling the miss in the inlined code. After the nop(1) instruction there
  // is a branch instruction for jumping back from the deferred code.
  Address address_after_call = address + Assembler::kCallTargetAddressOffset;
  Instr instr_after_call = Assembler::instr_at(address_after_call);
  if (!Assembler::is_nop(instr_after_call, PROPERTY_ACCESS_INLINED)) {
    return false;
  }
  Address address_after_nop = address_after_call + Assembler::kInstrSize;
  Instr instr_after_nop = Assembler::instr_at(address_after_nop);
  // There may be some reg-reg move and frame merging code to skip over before
  // the branch back from the DeferredReferenceGetKeyedValue code to the inlined
  // code.
  while (!Assembler::is_branch(instr_after_nop)) {
    address_after_nop += Assembler::kInstrSize;
    instr_after_nop = Assembler::instr_at(address_after_nop);
  }

  // Find the end of the inlined code for handling the load.
  int b_offset =
  Assembler::get_branch_offset(instr_after_nop) + Assembler::kPcLoadDelta;
  ASSERT(b_offset < 0);  // Jumping back from deferred code.
  *inline_end_address = address_after_nop + b_offset;

  return true;
}


void LoadIC::ClearInlinedVersion(Address address) {
  // Reset the map check of the inlined inobject property load (if present) to
  // guarantee failure by holding an invalid map (the null value). The offset
  // can be patched to anything.
  PatchInlinedLoad(address, Heap::null_value(), 0);
}


bool LoadIC::PatchInlinedLoad(Address address, Object* map, int offset) {
  // See CodeGenerator::EmitNamedLoad for
  // explanation of constants and instructions used here.

  // Find the end of the inlined code for handling the load if this is an
  // inlined IC call site.
  Address inline_end_address;
  if (!IsInlinedICSite(address, &inline_end_address)) return false;

  // Patch the offset of the property load instruction.
  // The immediate must be representable in 16 bits.
  ASSERT((JSObject::kMaxInstanceSize - JSObject::kHeaderSize) < (1 << 16));

  Address lw_property_instr_address =
          inline_end_address - Assembler::kInstrSize;
  ASSERT(Assembler::is_lw(Assembler::instr_at(lw_property_instr_address)));
  Instr lw_property_instr = Assembler::instr_at(lw_property_instr_address);

  lw_property_instr = Assembler::set_lw_offset(
      lw_property_instr, offset - kHeapObjectTag);
  Assembler::instr_at_put(lw_property_instr_address, lw_property_instr);

  // Indicate that code has changed.
  CPU::FlushICache(lw_property_instr_address, 1 * Assembler::kInstrSize);

  // Patch the map check.
  Address li_map_instr_address = inline_end_address - 5 * Assembler::kInstrSize;

  Assembler::set_target_address_at(li_map_instr_address,
                                   reinterpret_cast<Address>(map));
  return true;
}


void KeyedLoadIC::ClearInlinedVersion(Address address) {
  // Reset the map check of the inlined keyed load (if present) to
  // guarantee failure by holding an invalid map (the null value).
  PatchInlinedLoad(address, Heap::null_value());
}


bool KeyedLoadIC::PatchInlinedLoad(Address address, Object* map) {
  Address inline_end_address;
  if (!IsInlinedICSite(address, &inline_end_address)) return false;

  // Patch the map check.
  // This code patches CodeGenerator::EmitKeyedLoad(), at the
  // li(scratch2, Operand(Factory::null_value()), true); which at
  // present is 24 instructions from the end of the routine.
  Address ldr_map_instr_address =
      inline_end_address -
      (CodeGenerator::kInlinedKeyedLoadInstructionsAfterPatch *
      Assembler::kInstrSize);
  Assembler::set_target_address_at(ldr_map_instr_address,
                                   reinterpret_cast<Address>(map));
  return true;
}


void KeyedStoreIC::ClearInlinedVersion(Address address) {
  // Insert null as the elements map to check for.  This will make
  // sure that the elements fast-case map check fails so that control
  // flows to the IC instead of the inlined version.
  PatchInlinedStore(address, Heap::null_value());
}


void KeyedStoreIC::RestoreInlinedVersion(Address address) {
  // Restore the fast-case elements map check so that the inlined
  // version can be used again.
  PatchInlinedStore(address, Heap::fixed_array_map());
}


bool KeyedStoreIC::PatchInlinedStore(Address address, Object* map) {
  // Find the end of the inlined code for handling the store if this is an
  // inlined IC call site.
  Address inline_end_address;
  if (!IsInlinedICSite(address, &inline_end_address)) return false;

  // Patch the map check.
  // This code patches CodeGenerator::EmitKeyedStore(), at the
  // __ li(t1, Operand(Factory::fixed_array_map()), true);
  // which is 'kInlinedKeyedStoreInstructionsAfterPatch'
  // instructions from the end of the routine.
  Address ldr_map_instr_address =
      inline_end_address -
      (CodeGenerator::kInlinedKeyedStoreInstructionsAfterPatch *
      Assembler::kInstrSize);
  Assembler::set_target_address_at(ldr_map_instr_address,
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
  Label slow, check_string, index_smi, index_string;
  Label check_pixel_array, probe_dictionary, check_number_dictionary;

  Register key = a0;
  Register receiver = a1;

  // Check that the object isn't a smi.
  __ BranchOnSmi(receiver, &slow);
  // Get the map of the receiver.
  __ lw(a2, FieldMemOperand(receiver, HeapObject::kMapOffset));
  // Check bit field.
  __ lbu(a3, FieldMemOperand(a2, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(kSlowCaseBitFieldMask));
  __ Branch(&slow, ne, a3, Operand(zero_reg));
  // Check that the object is some kind of JS object EXCEPT JS Value type.
  // In the case that the object is a value-wrapper object,
  // we enter the runtime system to make sure that indexing into string
  // objects work as intended.
  ASSERT(JS_OBJECT_TYPE > JS_VALUE_TYPE);
  __ lbu(a2, FieldMemOperand(a2, Map::kInstanceTypeOffset));
  __ Branch(&slow, lt, a2, Operand(JS_OBJECT_TYPE));

  // Check that the key is a smi.
  __ BranchOnNotSmi(key, &check_string);
  __ bind(&index_smi);
  // Now the key is known to be a smi. This place is also jumped to from below
  // where a numeric string is converted to a smi.

  // Get the elements array of the object.
  __ lw(t0, FieldMemOperand(receiver, JSObject::kElementsOffset));
  // Check that the object is in fast mode (not dictionary).
  __ lw(a3, FieldMemOperand(t0, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kFixedArrayMapRootIndex);
  __ Branch(&check_pixel_array, ne, a3, Operand(at));
  // Check that the key (index) is within bounds.
  __ lw(a3, FieldMemOperand(t0, FixedArray::kLengthOffset));
  __ Branch(&slow, hs, key, Operand(a3));

  // Fast case: Do the load.
  // a0: key (tagged smi).
  // t0: elements array of the object.
  __ Addu(a3, t0, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  // The key is a smi.
  ASSERT(kSmiTag == 0 && kSmiTagSize < kPointerSizeLog2);
  __ sll(a2, key, kPointerSizeLog2 - kSmiTagSize);  // Scale index for words.
  __ Addu(a2, a2, Operand(a3));
  __ lw(v0, MemOperand(a2, 0));
  __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
  // In case the loaded value is the_hole we have to consult GetProperty
  // to ensure the prototype chain is searched.
  __ Branch(&slow, eq, v0, Operand(at));
  __ IncrementCounter(&Counters::keyed_load_generic_smi, 1, a2, a3);
  __ Ret();

  // Check whether the elements is a pixel array.
  // a0: key
  // a3: elements map
  // t0: elements
  __ bind(&check_pixel_array);
  __ LoadRoot(at, Heap::kPixelArrayMapRootIndex);
  __ Branch(&check_number_dictionary, ne, a3, Operand(at));
  // Check that the key (index) is within bounds.
  __ lw(at, FieldMemOperand(t0, PixelArray::kLengthOffset));
  __ sra(a2, key, kSmiTagSize);  // Untag the key.
  __ Branch(&slow, hs, a2, Operand(at));
  __ lw(at, FieldMemOperand(t0, PixelArray::kExternalPointerOffset));
  __ addu(at, at, a2);
  __ lbu(a2, MemOperand(at, 0));
  __ sll(v0, a2, kSmiTagSize);  // Tag result as smi.
  __ Ret();

  __ bind(&check_number_dictionary);
  // Check whether the elements is a number dictionary.
  // a0: key
  // a3: elements map
  // t0: elements
  __ LoadRoot(at, Heap::kHashTableMapRootIndex);
  __ Branch(&slow, ne, a3, Operand(at));
  __ sra(a2, a0, kSmiTagSize);
  GenerateNumberDictionaryLoad(masm, &slow, t0, a0, a2, a3, t1);
  __ mov(v0, a0);  // Return value in v0.
  __ Ret();

  // Slow case, key and receiver still in a0 and a1.
  __ bind(&slow);
  __ IncrementCounter(&Counters::keyed_load_generic_slow, 1, a2, a3);
  GenerateRuntimeGetProperty(masm);

  __ bind(&check_string);
  // The key is not a smi.
  // Is it a string?
  // a0: key (non-smi)
  // a1: receiver
  __ GetObjectType(a0, a2, a3);
  __ Branch(&slow, ge, a3, Operand(FIRST_NONSTRING_TYPE));

  // Is the string an array index, with cached numeric value?
  __ lw(a3, FieldMemOperand(a0, String::kHashFieldOffset));
  __ And(at, a3, Operand(String::kIsArrayIndexMask));
  __ Branch(&index_string, ne, at, Operand(zero_reg));

  // Is the string a symbol?
  // a2: key map
  __ lbu(a3, FieldMemOperand(a2, Map::kInstanceTypeOffset));
  ASSERT(kSymbolTag != 0);
  __ And(at, a3, Operand(kIsSymbolMask));
  __ Branch(&slow, eq, at, Operand(zero_reg));

  // If the receiver is a fast-case object, check the keyed lookup
  // cache. Otherwise probe the dictionary.
  __ lw(a3, FieldMemOperand(a1, JSObject::kPropertiesOffset));
  __ lw(a3, FieldMemOperand(a3, HeapObject::kMapOffset));
  __ LoadRoot(at, Heap::kHashTableMapRootIndex);
  __ Branch(&probe_dictionary, eq, a3, Operand(at));

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

  // Get field offset and check that it is an in-object property.
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
  __ Branch(&slow, ge, t1, Operand(t2));

  // Load in-object property.
  __ subu(t1, t1, t2);  // Index from end of object.
  __ lbu(t2, FieldMemOperand(a2, Map::kInstanceSizeOffset));
  __ addu(t2, t2, t1);  // Index from start of object.
  __ Subu(a1, a1, Operand(kHeapObjectTag));  // Remove the heap tag.
  __ sll(at, t2, kPointerSizeLog2);
  __ addu(at, a1, at);
  __ lw(v0, MemOperand(at));
  __ IncrementCounter(&Counters::keyed_load_generic_lookup_cache, 1, a2, a3);
  __ Ret();

  // Do a quick inline probe of the receiver's dictionary, if it
  // exists.
  __ bind(&probe_dictionary);
  // Load the property to v0.
  GenerateDictionaryLoad(
      masm, &slow, a1, a0, v0, a2, a3, t0, DICTIONARY_CHECK_DONE);
  __ IncrementCounter(&Counters::keyed_load_generic_symbol, 1, a2, a3);
  __ Ret();

  __ b(&slow);
  // If the hash field contains an array index pick it out. The assert checks
  // that the constants for the maximum number of digits for an array index
  // cached in the hash field and the number of bits reserved for it does not
  // conflict.
  ASSERT(TenToThe(String::kMaxCachedArrayIndexLength) <
         (1 << String::kArrayIndexValueBits));
  __ bind(&index_string);
  // a0: key (string)
  // a1: receiver
  // a3: hash field
  // We want the smi-tagged index in a0.  kArrayIndexValueMask has zeros in
  // the low kHashShift bits.
  ASSERT(String::kHashShift >= kSmiTagSize);
  __ And(a3, a3, Operand(String::kArrayIndexValueMask));
  // Here we actually clobber the key (a0) which will be used if calling into
  // runtime later. However as the new key is the numeric value of a string key
  // there is no difference in using either key.
  __ sra(a0, a3, String::kHashShift - kSmiTagSize);
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
  Label index_out_of_range;

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
                                          &index_out_of_range,
                                          STRING_INDEX_IS_ARRAY_INDEX);
  char_at_generator.GenerateFast(masm);
  __ Ret();

  ICRuntimeCallHelper call_helper;
  char_at_generator.GenerateSlow(masm, call_helper);

  __ bind(&index_out_of_range);
  __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
  __ Ret();

  __ bind(&miss);
  GenerateMiss(masm);
}


// Convert unsigned integer with specified number of leading zeroes in binary
// representation to IEEE 754 double.
// Integer to convert is passed in register hiword.
// Resulting double is returned in registers hiword:loword.
// This functions does not work correctly for 0.
static void GenerateUInt2Double(MacroAssembler* masm,
                                Register hiword,
                                Register loword,
                                Register scratch,
                                int leading_zeroes) {
  const int meaningful_bits = kBitsPerInt - leading_zeroes - 1;
  const int biased_exponent = HeapNumber::kExponentBias + meaningful_bits;

  const int mantissa_shift_for_hi_word =
      meaningful_bits - HeapNumber::kMantissaBitsInTopWord;

  const int mantissa_shift_for_lo_word =
      kBitsPerInt - mantissa_shift_for_hi_word;

  __ li(scratch, biased_exponent << HeapNumber::kExponentShift);
  if (mantissa_shift_for_hi_word > 0) {
    __ sll(loword, hiword, mantissa_shift_for_lo_word);
    __ srl(hiword, hiword, mantissa_shift_for_hi_word);
    __ or_(hiword, scratch, hiword);
  } else {
    __ mov(loword, zero_reg);
    __ sll(hiword, hiword, mantissa_shift_for_hi_word);
    __ or_(hiword, scratch, hiword);
  }

  // If least significant bit of biased exponent was not 1 it was corrupted
  // by most significant bit of mantissa so we should fix that.
  if (!(biased_exponent & 1)) {
    __ li(scratch, 1 << HeapNumber::kExponentShift);
    __ nor(scratch, scratch, scratch);
    __ and_(hiword, hiword, scratch);
  }
}

void KeyedLoadIC::GenerateExternalArray(MacroAssembler* masm,
                                        ExternalArrayType array_type) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- a1     : receiver
  // -----------------------------------
  Label slow, failed_allocation;

  Register key = a0;
  Register receiver = a1;

  // Check that the object isn't a smi
  __ BranchOnSmi(receiver, &slow);

  // Check that the key is a smi.
  __ BranchOnNotSmi(key, &slow);

  // Check that the object is a JS object. Load map into a2.
  __ GetObjectType(receiver, a2, a3);
  __ Branch(&slow, lt, a3, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that the receiver does not require access checks.  We need
  // to check this explicitly since this generic stub does not perform
  // map checks.
  __ lbu(a3, FieldMemOperand(a2, Map::kBitFieldOffset));
  __ And(t1, a3, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&slow, ne, t1, Operand(zero_reg));

  // Check that the elements array is the appropriate type of
  // ExternalArray.
  __ lw(a3, FieldMemOperand(receiver, JSObject::kElementsOffset));
  __ lw(a2, FieldMemOperand(a3, HeapObject::kMapOffset));
  __ LoadRoot(t1, Heap::RootIndexForExternalArrayType(array_type));
  __ Branch(&slow, ne, a2, Operand(t1));

  // Check that the index is in range.
  __ lw(t1, FieldMemOperand(a3, ExternalArray::kLengthOffset));
  __ sra(t2, key, kSmiTagSize);
  // Unsigned comparison catches both negative and too-large values.
  __ Branch(&slow, Uless, t1, Operand(t2));


  // a3: elements array
  __ lw(a3, FieldMemOperand(a3, ExternalArray::kExternalPointerOffset));
  // a3: base pointer of external storage

  // We are not untagging smi key and instead work with it
  // as if it was premultiplied by 2.
  ASSERT((kSmiTag == 0) && (kSmiTagSize == 1));

  Register value = a2;
  switch (array_type) {
    case kExternalByteArray:
      __ addu(t3, a3, t2);
      __ lb(value, MemOperand(t3, 0));
      break;
    case kExternalUnsignedByteArray:
      __ addu(t3, a3, t2);
      __ lbu(value, MemOperand(t3, 0));
      break;
    case kExternalShortArray:
      __ sll(t3, t2, 1);
      __ addu(t3, a3, t3);
      __ lh(value, MemOperand(t3, 0));
      break;
    case kExternalUnsignedShortArray:
      __ sll(t3, t2, 1);
      __ addu(t3, a3, t3);
      __ lhu(value, MemOperand(t3, 0));
      break;
    case kExternalIntArray:
    case kExternalUnsignedIntArray:
      __ sll(t3, t2, 2);
      __ addu(t3, a3, t3);
      __ lw(value, MemOperand(t3, 0));
      break;
    case kExternalFloatArray:
      if (CpuFeatures::IsSupported(FPU)) {
        CpuFeatures::Scope scope(FPU);
        __ sll(t3, t2, 2);
        __ addu(a2, a3, t3);
        __ lwc1(f0, MemOperand(a2, 0));
      } else {
        __ sll(t3, t2, 2);
        __ addu(t3, a3, t3);
        __ lw(value, MemOperand(t3, 0));
      }
      break;
    default:
      UNREACHABLE();
      break;
  }

  // For integer array types:
  // a2: value
  // For floating-point array type
  // f0: value (if FPU is supported)
  // a2: value (if FPU is not supported)

  if (array_type == kExternalIntArray) {
    // For the Int and UnsignedInt array types, we need to see whether
    // the value can be represented in a Smi. If not, we need to convert
    // it to a HeapNumber.
    Label box_int;
    __ Subu(t3, value, Operand(0xC0000000));  // Non-smi value gives neg result.
    __ Branch(&box_int, lt, t3, Operand(zero_reg));
    // Tag integer as smi and return it.
    __ sll(v0, value, kSmiTagSize);
    __ Ret();

    __ bind(&box_int);
    // Allocate a HeapNumber for the result and perform int-to-double
    // conversion. Use v0 for result as key is not needed any more.
    __ AllocateHeapNumber(v0, a3, t0, &slow);

    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      __ mtc1(value, f0);
      __ cvt_d_w(f0, f0);
      __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset - kHeapObjectTag));
      __ Ret();
    } else {
      WriteInt32ToHeapNumberStub stub(value, v0, t2, t3);
      __ TailCallStub(&stub);
    }
  } else if (array_type == kExternalUnsignedIntArray) {
    // The test is different for unsigned int values. Since we need
    // the value to be in the range of a positive smi, we can't
    // handle either of the top two bits being set in the value.
    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      Label pl_box_int;
      __ And(t2, value, Operand(0xC0000000));
      __ Branch(&pl_box_int, ne, t2, Operand(zero_reg));

      // It can fit in an Smi.
      // Tag integer as smi and return it.
      __ sll(v0, value, kSmiTagSize);
      __ Ret();

      __ bind(&pl_box_int);
      // Allocate a HeapNumber for the result and perform int-to-double
      // conversion. Don't use a0 and a1 as AllocateHeapNumber clobbers all
      // registers - also when jumping due to exhausted young space.
      __ AllocateHeapNumber(v0, t2, t3, &slow);
      __ mtc1(value, f0);     // LS 32-bits.
      __ mtc1(zero_reg, f1);  // MS 32-bits are all zero.
      __ cvt_d_l(f0, f0);     // Use 64 bit conv to get correct unsigned 32-bit.
      __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset - kHeapObjectTag));

      __ Ret();
    } else {
      // Check whether unsigned integer fits into smi.
      Label box_int_0, box_int_1, done;
      __ And(t2, value, Operand(0x80000000));
      __ Branch(&box_int_0, ne, t2, Operand(zero_reg));
      __ And(t2, value, Operand(0x40000000));
      __ Branch(&box_int_1, ne, t2, Operand(zero_reg));

      // Tag integer as smi and return it.
      __ sll(v0, value, kSmiTagSize);
      __ Ret();

      Register hiword = value;  // a2.
      Register loword = a3;

      __ bind(&box_int_0);
      // Integer does not have leading zeros.
      GenerateUInt2Double(masm, hiword, loword, t0, 0);
      __ b(&done);

      __ bind(&box_int_1);
      // Integer has one leading zero.
      GenerateUInt2Double(masm, hiword, loword, t0, 1);


      __ bind(&done);
      // Integer was converted to double in registers hiword:loword.
      // Wrap it into a HeapNumber. Don't use a0 and a1 as AllocateHeapNumber
      // clobbers all registers - also when jumping due to exhausted young
      // space.
      __ AllocateHeapNumber(t2, t3, t5, &slow);

      __ sw(hiword, FieldMemOperand(t2, HeapNumber::kExponentOffset));
      __ sw(loword, FieldMemOperand(t2, HeapNumber::kMantissaOffset));

      __ mov(v0, t2);
      __ Ret();
    }
  } else if (array_type == kExternalFloatArray) {
    // For the floating-point array type, we need to always allocate a
    // HeapNumber.
    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      // Allocate a HeapNumber for the result. Don't use a0 and a1 as
      // AllocateHeapNumber clobbers all registers - also when jumping due to
      // exhausted young space.
      __ AllocateHeapNumber(v0, t3, t5, &slow);
      // The float (single) value is already in fpu reg f0 (if we use float).
      __ cvt_d_s(f0, f0);
      __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset - kHeapObjectTag));
      __ Ret();
    } else {
      // Allocate a HeapNumber for the result. Don't use a0 and a1 as
      // AllocateHeapNumber clobbers all registers - also when jumping due to
      // exhausted young space.
      __ AllocateHeapNumber(v0, t3, t5, &slow);
      // FPU is not available, do manual single to double conversion.

      // a2: floating point value (binary32).
      // v0: heap number for result

      // Extract mantissa to t4.
      __ And(t4, value, Operand(kBinary32MantissaMask));

      // Extract exponent to t5.
      __ srl(t5, value, kBinary32MantissaBits);
      __ And(t5, t5, Operand(kBinary32ExponentMask >> kBinary32MantissaBits));

      Label exponent_rebiased;
      __ Branch(&exponent_rebiased, eq, t5, Operand(zero_reg));

      __ li(t0, 0x7ff);
      __ Xor(t1, t5, Operand(0xFF));
      __ movz(t5, t0, t1);  // Set t5 to 0x7ff only if t5 is equal to 0xff.
      __ Branch(&exponent_rebiased, eq, t0, Operand(0xff));

      // Rebias exponent.
      __ Addu(t5,
              t5,
              Operand(-kBinary32ExponentBias + HeapNumber::kExponentBias));

      __ bind(&exponent_rebiased);
      __ And(a2, value, Operand(kBinary32SignMask));
      __ sll(t0, t0, HeapNumber::kMantissaBitsInTopWord);
      value = no_reg;
      __ or_(a2, a2, t0);

      // Shift mantissa.
      static const int kMantissaShiftForHiWord =
          kBinary32MantissaBits - HeapNumber::kMantissaBitsInTopWord;

      static const int kMantissaShiftForLoWord =
          kBitsPerInt - kMantissaShiftForHiWord;

      __ srl(t0, t4, kMantissaShiftForHiWord);
      __ or_(a2, a2, t0);
      __ sll(a0, t4, kMantissaShiftForLoWord);

      __ sw(a2, FieldMemOperand(v0, HeapNumber::kExponentOffset));
      __ sw(a0, FieldMemOperand(v0, HeapNumber::kMantissaOffset));
      __ Ret();
    }

  } else {
    // Tag integer as smi and return it.
    __ sll(v0, value, kSmiTagSize);
    __ Ret();
  }

  // Slow case, key and receiver still in a0 and a1.
  __ bind(&slow);
  __ IncrementCounter(&Counters::keyed_load_external_array_slow, 1, a2, a3);
  GenerateRuntimeGetProperty(masm);
}


void KeyedStoreIC::GenerateRuntimeSetProperty(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- a1     : key
  //  -- a2     : receiver
  //  -- ra     : return address
  // -----------------------------------

  // Push receiver, key and value for runtime call.
  __ Push(a2, a1, a0);
  __ TailCallRuntime(Runtime::kSetProperty, 3, 1);
}


void KeyedStoreIC::GenerateGeneric(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- a1     : key
  //  -- a2     : receiver
  //  -- ra     : return address
  // -----------------------------------

  Label slow, fast, array, extra, check_pixel_array, exit;

  // Register usage.
  Register value = a0;
  Register key = a1;
  Register receiver = a2;
  Register elements = a3;  // Elements array of the receiver.
  // t0 is used as ip in the arm version.
  // t3-t4 are used as temporaries.

  // Check that the key is a smi.
  __ BranchOnNotSmi(key, &slow);
  // Check that the object isn't a smi.
  __ BranchOnSmi(receiver, &slow);

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
  // Check that the object is in fast mode (not dictionary).
  __ lw(t3, FieldMemOperand(elements, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::kFixedArrayMapRootIndex);
  __ Branch(&check_pixel_array, ne, t3, Operand(t0));
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

  GenerateRuntimeSetProperty(masm);

  // Check whether the elements is a pixel array.
  // t3: elements map

  __ bind(&check_pixel_array);
  __ LoadRoot(t0, Heap::kPixelArrayMapRootIndex);
  __ Branch(&slow, ne, t3, Operand(t0));
  // Check that the value is a smi. If a conversion is needed call into the
  // runtime to convert and clamp.
  __ BranchOnNotSmi(value, &slow);
  __ sra(t3, key, kSmiTagSize);  // Untag the key.
  __ lw(t0, FieldMemOperand(elements, PixelArray::kLengthOffset));
  __ Branch(&slow, hs, t3, Operand(t0));
  __ sra(t4, value, kSmiTagSize);  // Untag the value.
  {  // Clamp the value to [0..255].
     // To my understanding the following piece of code sets t4 to:
     //    0 if t4 < 0
     //    255 if t4 > 255
     //    t4 otherwise
     // This is done by using v0 as a temporary (not as a return value).
    Label done;
    __ li(v0, Operand(255));
    __ Branch(&done, gt, t4, Operand(v0));  // Normal: nop in delay slot.
    __ Branch(false, &done, lt, a0, Operand(zero_reg));  // Use delay slot.
    __ mov(v0, zero_reg);  // In delay slot.
    __ mov(v0, t4);  // Value is in range 0..255.
    __ bind(&done);
    __ mov(t4, v0);
  }

  // Get the pointer to the external array. This clobbers elements.
  __ lw(elements,
        FieldMemOperand(elements, PixelArray::kExternalPointerOffset));
  __ Addu(t1, elements, t3);  // Base + index.
  __ sb(t4, MemOperand(t1, 0));  // Elements is now external array.

  __ mov(v0, value);
  __ Ret();


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
  __ Branch(&fast, al);


  // Array case: Get the length and the elements array from the JS
  // array. Check that the array is in fast mode; if it is the
  // length is always a smi.

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
  __ BranchOnSmi(value, &exit);

  // Update write barrier for the elements array address.
  __ Subu(t3, t4, Operand(elements));

  __ RecordWrite(elements, t3, t4);
  __ bind(&exit);

  __ mov(v0, a0);  // Return the value written.
  __ Ret();
}


// Convert int passed in register ival to IEEE-754 single precision
// floating point value and store it into register fval.
// If FPU is available use it for conversion.
static void ConvertIntToFloat(MacroAssembler* masm,
                              Register ival,
                              Register fval,
                              Register scratch1,
                              Register scratch2) {
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ mtc1(ival, f0);
    __ cvt_s_w(f0, f0);
    __ mfc1(fval, f0);
  } else {
    // FPU is not available,  do manual conversions.

    Label not_special, done;
    // Move sign bit from source to destination.  This works because the sign
    // bit in the exponent word of the double has the same position and polarity
    // as the 2's complement sign bit in a Smi.
    ASSERT(kBinary32SignMask == 0x80000000u);

    __ And(fval, ival, Operand(kBinary32SignMask));
    // Negate value if it is negative.
    __ subu(scratch1, zero_reg, ival);
    __ movn(ival, scratch1, fval);

    // We have -1, 0 or 1, which we treat specially. Register ival contains
    // absolute value: it is either equal to 1 (special case of -1 and 1),
    // greater than 1 (not a special case) or less than 1 (special case of 0).
    __ Branch(&not_special, gt, ival, Operand(1));

    // For 1 or -1 we need to or in the 0 exponent (biased).
    static const uint32_t exponent_word_for_1 =
        kBinary32ExponentBias << kBinary32ExponentShift;

    __ Xor(scratch1, ival, Operand(1));
    __ li(scratch2, exponent_word_for_1);
    __ or_(scratch2, fval, scratch2);
    __ movz(fval, scratch2, scratch1);  // Only if ival is equal to 1.
    __ Branch(&done);

    __ bind(&not_special);
    // Count leading zeros.
    // Gets the wrong answer for 0, but we already checked for that case above.
    Register zeros = scratch2;
    __ clz(zeros, ival);

    // Compute exponent and or it into the exponent register.
    __ li(scratch1, (kBitsPerInt - 1) + kBinary32ExponentBias);
    __ subu(scratch1, scratch1, zeros);

    __ sll(scratch1, scratch1, kBinary32ExponentShift);
    __ or_(fval, fval, scratch1);

    // Shift up the source chopping the top bit off.
    __ Addu(zeros, zeros, Operand(1));
    // This wouldn't work for 1 and -1 as the shift would be 32 which means 0.
    __ sllv(ival, ival, zeros);
    // And the top (top 20 bits).
    __ srl(scratch1, ival, kBitsPerInt - kBinary32MantissaBits);
    __ or_(fval, fval, scratch1);

    __ bind(&done);
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
  //  -- a1     : key
  //  -- a2     : receiver
  //  -- ra     : return address
  // -----------------------------------

  Label slow, check_heap_number;

  // Register usage.
  Register value = a0;
  Register key = a1;
  Register receiver = a2;
  // a3 mostly holds the elements array or the destination external array.

  // Check that the object isn't a smi.
  __ BranchOnSmi(receiver, &slow);

  // Check that the object is a JS object. Load map into a3.
  __ GetObjectType(receiver, a3, t0);
  __ Branch(&slow, lt, t0, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that the receiver does not require access checks.  We need
  // to do this because this generic stub does not perform map checks.
  __ lbu(t1, FieldMemOperand(a3, Map::kBitFieldOffset));
  __ And(t1, t1, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&slow, ne, t1, Operand(zero_reg));

  // Check that the key is a smi.
  __ BranchOnNotSmi(key, &slow);

  // Check that the elements array is the appropriate type of ExternalArray.
  __ lw(a3, FieldMemOperand(a2, JSObject::kElementsOffset));
  __ lw(t0, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ LoadRoot(t1, Heap::RootIndexForExternalArrayType(array_type));
  __ Branch(&slow, ne, t0, Operand(t1));

  // Check that the index is in range.
  __ sra(t0, key, kSmiTagSize);  // Untag the index.
  __ lw(t1, FieldMemOperand(a3, ExternalArray::kLengthOffset));
  // Unsigned comparison catches both negative and too-large values.
  __ Branch(&slow, Ugreater_equal, t0, Operand(t1));

  // Handle both smis and HeapNumbers in the fast path. Go to the
  // runtime for all other kinds of values.
  // a3: external array.
  // t0: key (integer).

  __ BranchOnNotSmi(value, &check_heap_number);
  __ sra(t1, value, kSmiTagSize);  // Untag the value.
  __ lw(a3, FieldMemOperand(a3, ExternalArray::kExternalPointerOffset));

  // a3: base pointer of external storage.
  // t0: key (integer).
  // t1: value (integer).

  switch (array_type) {
    case kExternalByteArray:
    case kExternalUnsignedByteArray:
      __ addu(t8, a3, t0);
      __ sb(t1, MemOperand(t8, 0));
      break;
    case kExternalShortArray:
    case kExternalUnsignedShortArray:
      __ sll(t8, t0, 1);
      __ addu(t8, a3, t8);
      __ sh(t1, MemOperand(t8, 0));
      break;
    case kExternalIntArray:
    case kExternalUnsignedIntArray:
      __ sll(t8, t0, 2);
      __ addu(t8, a3, t8);
      __ sw(t1, MemOperand(t8, 0));
      break;
    case kExternalFloatArray:
      // Need to perform int-to-float conversion.
      ConvertIntToFloat(masm, t1, t2, t3, t4);
      __ sll(t8, t0, 2);
      __ addu(t8, a3, t8);
      __ sw(t1, MemOperand(t8, 0));
      break;
    default:
      UNREACHABLE();
      break;
  }

  // Entry registers are intact, a0 holds the value which is the return value.
  __ mov(v0, value);
  __ Ret();

  // a3: external array.
  // t0: index (integer).
  __ bind(&check_heap_number);
  __ GetObjectType(value, t1, t2);
  __ Branch(&slow, ne, t2, Operand(HEAP_NUMBER_TYPE));

  __ lw(a3, FieldMemOperand(a3, ExternalArray::kExternalPointerOffset));

  // a3: base pointer of external storage.
  // t0: key (integer).

  // The WebGL specification leaves the behavior of storing NaN and
  // +/-Infinity into integer arrays basically undefined. For more
  // reproducible behavior, convert these to zero.

  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);

    __ ldc1(f0, MemOperand(a0, HeapNumber::kValueOffset - kHeapObjectTag));

    if (array_type == kExternalFloatArray) {
      __ cvt_s_d(f0, f0);
      __ sll(t8, t0, 2);
      __ addu(t8, a3, t8);
      __ swc1(f0, MemOperand(t8, 0));
    } else {
      Label done;

      // Need to perform float-to-int conversion.
      // Test whether exponent equal to 0x7FF (infinity or NaN).

      __ mfc1(t3, f1);  // Move exponent word of double to t3 (as raw bits).
      __ li(t1, Operand(0x7FF00000));
      __ And(t3, t3, Operand(t1));
      __ Branch(false, &done, eq, t3, Operand(t1));
      __ mov(t3, zero_reg);  // In delay slot.

      // Not infinity or NaN simply convert to int.
      if (IsElementTypeSigned(array_type)) {
        __ trunc_w_d(f0, f0);
        __ mfc1(t3, f0);
      } else {
        __ trunc_l_d(f0, f0);  // Convert double to 64-bit int.
        __ mfc1(t3, f0);  // Keep the LS 32-bits.
      }

      // t3: HeapNumber converted to integer
      __ bind(&done);
      switch (array_type) {
        case kExternalByteArray:
        case kExternalUnsignedByteArray:
          __ addu(t8, a3, t0);
          __ sb(t3, MemOperand(t8, 0));
          break;
        case kExternalShortArray:
        case kExternalUnsignedShortArray:
          __ sll(t8, t0, 1);
          __ addu(t8, a3, t8);
          __ sh(t3, MemOperand(t8, 0));
          break;
        case kExternalIntArray:
        case kExternalUnsignedIntArray:
          __ sll(t8, t0, 2);
          __ addu(t8, a3, t8);
          __ sw(t3, MemOperand(t8, 0));
          break;
        default:
          UNREACHABLE();
          break;
      }
    }

    // Entry registers are intact, a0 holds the value which is the return value.
    __ mov(v0, value);
    __ Ret();
  } else {
    // FPU is not available, do manual conversions.

    __ lw(t3, FieldMemOperand(value, HeapNumber::kExponentOffset));
    __ lw(t4, FieldMemOperand(value, HeapNumber::kMantissaOffset));

    if (array_type == kExternalFloatArray) {
      Label done, nan_or_infinity_or_zero;
      static const int kMantissaInHiWordShift =
          kBinary32MantissaBits - HeapNumber::kMantissaBitsInTopWord;

      static const int kMantissaInLoWordShift =
          kBitsPerInt - kMantissaInHiWordShift;

      // Test for all special exponent values: zeros, subnormal numbers, NaNs
      // and infinities. All these should be converted to 0.
      __ li(t5, HeapNumber::kExponentMask);
      __ and_(t6, t3, t5);
      __ Branch(&nan_or_infinity_or_zero, eq, t6, Operand(zero_reg));

      __ xor_(t1, t6, t5);
      __ li(t2, kBinary32ExponentMask);
      __ movz(t6, t2, t1);  // Only if t6 is equal to t5.
      __ Branch(&nan_or_infinity_or_zero, eq, t6, Operand(t5));

      // Rebias exponent.
      __ srl(t6, t6, HeapNumber::kExponentShift);
      __ Addu(t6,
              t6,
              Operand(kBinary32ExponentBias - HeapNumber::kExponentBias));

      __ li(t1, Operand(kBinary32MaxExponent));
      __ Slt(t1, t1, t6);
      __ And(t2, t3, Operand(HeapNumber::kSignMask));
      __ Or(t2, t2, Operand(kBinary32ExponentMask));
      __ movn(t3, t2, t1);  // Only if t6 is gt kBinary32MaxExponent.
      __ Branch(&done, gt, t6, Operand(kBinary32MaxExponent));

      __ Slt(t1, t6, Operand(kBinary32MinExponent));
      __ And(t2, t3, Operand(HeapNumber::kSignMask));
      __ movn(t3, t2, t1);  // Only if t6 is lt kBinary32MinExponent.
      __ Branch(&done, lt, t6, Operand(kBinary32MinExponent));

      __ And(t7, t3, Operand(HeapNumber::kSignMask));
      __ And(t3, t3, Operand(HeapNumber::kMantissaMask));
      __ sll(t3, t3, kMantissaInHiWordShift);
      __ or_(t7, t7, t3);
      __ srl(t4, t4, kMantissaInLoWordShift);
      __ or_(t7, t7, t4);
      __ sll(t6, t6, kBinary32ExponentShift);
      __ or_(t3, t7, t6);

      __ bind(&done);
      __ sll(t9, a1, 2);
      __ addu(t9, a2, t9);
      __ sw(t3, MemOperand(t9, 0));

      // Entry registers are intact, a0 holds the value which is the return
      // value.
      __ mov(v0, value);
      __ Ret();

      __ bind(&nan_or_infinity_or_zero);
      __ And(t7, t3, Operand(HeapNumber::kSignMask));
      __ And(t3, t3, Operand(HeapNumber::kMantissaMask));
      __ or_(t6, t6, t7);
      __ sll(t3, t3, kMantissaInHiWordShift);
      __ or_(t6, t6, t3);
      __ srl(t4, t4, kMantissaInLoWordShift);
      __ or_(t3, t6, t4);
      __ Branch(&done);
    } else {
      bool is_signed_type  = IsElementTypeSigned(array_type);
      int meaningfull_bits = is_signed_type ? (kBitsPerInt - 1) : kBitsPerInt;
      int32_t min_value    = is_signed_type ? 0x80000000 : 0x00000000;

      Label done, sign;

      // Test for all special exponent values: zeros, subnormal numbers, NaNs
      // and infinities. All these should be converted to 0.
      __ li(t5, HeapNumber::kExponentMask);
      __ and_(t6, t3, t5);
      __ movz(t3, zero_reg, t6);  // Only if t6 is equal to zero.
      __ Branch(&done, eq, t6, Operand(zero_reg));

      __ xor_(t2, t6, t5);
      __ movz(t3, zero_reg, t2);  // Only if t6 is equal to t5
      __ Branch(&done, eq, t6, Operand(t5));

      // Unbias exponent.
      __ srl(t6, t6, HeapNumber::kExponentShift);
      __ Subu(t6, t6, Operand(HeapNumber::kExponentBias));
      // If exponent is negative than result is 0.
      __ slt(t2, t6, zero_reg);
      __ movn(t3, zero_reg, t2);  // Only if exponent is negative.
      __ Branch(&done, lt, t6, Operand(zero_reg));

      // If exponent is too big than result is minimal value.
      __ slti(t1, t6, meaningfull_bits - 1);
      __ li(t2, min_value);
      __ movz(t3, t2, t1);  // Only if t6 is ge meaningfull_bits - 1.
      __ Branch(&done, ge, t6, Operand(meaningfull_bits - 1));

      __ And(t5, t3, Operand(HeapNumber::kSignMask));
      __ And(t3, t3, Operand(HeapNumber::kMantissaMask));
      __ Or(t3, t3, Operand(1u << HeapNumber::kMantissaBitsInTopWord));

      __ li(t9, HeapNumber::kMantissaBitsInTopWord);
      __ subu(t6, t9, t6);
      __ slt(t1, t6, zero_reg);
      __ srlv(t2, t3, t6);
      __ movz(t3, t2, t1);  // Only if t6 is positive.
      __ Branch(&sign, ge, t6, Operand(zero_reg));

      __ subu(t6, zero_reg, t6);
      __ sllv(t3, t3, t6);
      __ li(t9, meaningfull_bits);
      __ subu(t6, t9, t6);
      __ srlv(t4, t4, t6);
      __ or_(t3, t3, t4);

      __ bind(&sign);
      __ subu(t2, t3, zero_reg);
      __ movz(t3, t2, t5);  // Only if t5 is zero.

      __ bind(&done);

      // Result is in t3.
      // This switch block should be exactly the same as above (FPU mode).
      switch (array_type) {
        case kExternalByteArray:
        case kExternalUnsignedByteArray:
          __ addu(t8, a3, t0);
          __ sb(t3, MemOperand(t8, 0));
          break;
        case kExternalShortArray:
        case kExternalUnsignedShortArray:
          __ sll(t8, t0, 1);
          __ addu(t8, a3, t8);
          __ sh(t3, MemOperand(t8, 0));
          break;
        case kExternalIntArray:
        case kExternalUnsignedIntArray:
          __ sll(t8, t0, 2);
          __ addu(t8, a3, t8);
          __ sw(t3, MemOperand(t8, 0));
          break;
        default:
          UNREACHABLE();
          break;
      }
    }
  }

  // Slow case: call runtime.
  __ bind(&slow);
  // Entry registers are intact.
  GenerateRuntimeSetProperty(masm);
}


void KeyedLoadIC::GenerateIndexedInterceptor(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- a1     : receiver
  // -----------------------------------
  Label slow;

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
  __ Push(receiver, value);

  ExternalReference ref = ExternalReference(IC_Utility(kStoreIC_ArrayLength));
  __ TailCallExternalReference(ref, 2, 1);

  __ bind(&miss);

  GenerateMiss(masm);
}

#undef __

} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_MIPS
