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
  // t0 - scratch.
  // t1 - scratch.

  Label done;

  // Check for the absence of an interceptor.
  // Load the map into reg0.
  __ lw(reg0, FieldMemOperand(reg1, JSObject::kMapOffset));

  // Bail out if the receiver has a named interceptor.
  __ lbu(a3, FieldMemOperand(reg0, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(1 << Map::kHasNamedInterceptor));
  __ Branch(miss, ne, a3, Operand(zero_reg));

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
    __ lw(t1, FieldMemOperand(a2, String::kHashFieldOffset));
    if (i > 0) {
      // Add the probe offset (i + i * i) left shifted to avoid right shifting
      // the hash in a separate instruction. The value hash + i + i * i is right
      // shifted in the following and instruction.
      ASSERT(StringDictionary::GetProbeOffset(i) <
             1 << (32 - String::kHashFieldOffset));
      __ Addu(t1, t1, Operand(
          StringDictionary::GetProbeOffset(i) << String::kHashShift));
    }

    __ srl(t1, t1, String::kHashShift);
    __ And(t1, a3, t1);

    // Scale the index by multiplying by the element size.
    ASSERT(StringDictionary::kEntrySize == 3);
    __ sll(at, t1, 1);  // at = t1 * 2.
    __ addu(t1, t1, at);  // t1 = t1 * 3.

    // Check if the key is identical to the name.
    __ sll(at, t1, 2);
    __ addu(t1, reg0, at);
    __ lw(t0, FieldMemOperand(t1, kElementsStartOffset));
    if (i != kProbes - 1) {
      __ Branch(&done, eq, a2, Operand(t0));
    } else {
      __ Branch(miss, ne, a2, Operand(t0));
    }
  }

  // Check that the value is a normal property.
  __ bind(&done);  // t1 == reg0 + 4*index
  __ lw(a3, FieldMemOperand(t1, kElementsStartOffset + 2 * kPointerSize));
  __ And(a3, a3, Operand(PropertyDetails::TypeField::mask() << kSmiTagSize));
  __ Branch(miss, ne, a3, Operand(zero_reg));

  // Get the value at the masked, scaled index and return.
  __ lw(reg1, FieldMemOperand(t1, kElementsStartOffset + 1 * kPointerSize));
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
  //
  // Scratch registers:
  //
  // reg0 - holds the untagged key on entry and holds the hash once computed.
  //      Holds the result on exit if the load succeeded.
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
  __ lw(reg0, FieldMemOperand(reg2, kValueOffset));
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
  // r2    : name
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
  // r2    : name
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
  // This function uses name in a2, and trashes a3.
  GenerateDictionaryLoad(masm, miss, a0, a1);

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
  // This function uses name in a2, and trashes a3.
  GenerateDictionaryLoad(masm, &miss, a1, a0);
  __ mov(v0, a0);
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
  // a0    : receiver
  // sp[0] : receiver

  __ mov(a3, a0);
  __ MultiPush(a2.bit() | a3.bit());

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
  ASSERT(Assembler::is_branch(instr_after_nop));

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
          inline_end_address - 24 * Assembler::kInstrSize;
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
  // __ li(t1, Operand(Factory::fixed_array_map()), true);  which at
  // present is 9 instructions from the end of the routine.
  Address ldr_map_instr_address =
      inline_end_address - 9 * Assembler::kInstrSize;
  Assembler::set_target_address_at(ldr_map_instr_address,
                                   reinterpret_cast<Address>(map));
  return true;
}


Object* KeyedLoadIC_Miss(Arguments args);


void KeyedLoadIC::GenerateMiss(MacroAssembler* masm) {
  // ra     : return address
  // a0     : key
  // sp[0]  : key
  // sp[4]  : receiver

  __ lw(a1, MemOperand(sp, kPointerSize));
  __ MultiPush(a1.bit() | a0.bit());

  ExternalReference ref = ExternalReference(IC_Utility(kKeyedLoadIC_Miss));
  __ TailCallExternalReference(ref, 2, 1);
}


void KeyedLoadIC::GenerateRuntimeGetProperty(MacroAssembler* masm) {
  // ra     : return address
  // a0     : key
  // sp[0]  : key
  // sp[4]  : receiver

  __ lw(a1, MemOperand(sp, kPointerSize));
  __ MultiPush(a1.bit() | a0.bit());
  // Do a tail-call to runtime routine.

  __ TailCallRuntime(Runtime::kGetProperty, 2, 1);
}


void KeyedLoadIC::GenerateGeneric(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- sp[0]  : key
  //  -- sp[4]  : receiver
  // -----------------------------------
  Label slow, check_pixel_array, check_number_dictionary;

  // Modified slightly from in-tree arm version, see
  // ic-arm.cc: 6a579d9fd7.

  // Get the object from the stack.
  __ lw(a1, MemOperand(sp, kPointerSize));

  // a0: key
  // a1: receiver object

  // Check that the object isn't a smi.
  __ BranchOnSmi(a1, &slow);
  // Get the map of the receiver.
  __ lw(a2, FieldMemOperand(a1, HeapObject::kMapOffset));
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
  __ BranchOnNotSmi(a0, &slow);
  // Save key in a2 in case we want it for the number dictionary case.
  __ mov(a2, a0);
  __ sra(a0, a0, kSmiTagSize);  // Untag the key.

  // Get the elements array of the object.
  __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));
  // Check that the object is in fast mode (not dictionary).
  __ lw(a3, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::kFixedArrayMapRootIndex);
  __ Branch(&check_pixel_array, ne, a3, Operand(t0));
  // Check that the key (index) is within bounds.
  __ lw(a3, FieldMemOperand(a1, Array::kLengthOffset));
  __ Branch(&slow, hs, a0, Operand(a3));

  // Fast case: Do the load.
  // a0: key (un-tagged integer).
  // a1: elements array of the object.
  __ Addu(a3, a1, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t0, a0, kPointerSizeLog2);  // Scale index for words.
  __ Addu(t0, t0, Operand(a3));  // Index + base.
  __ lw(v0, MemOperand(t0, 0));
  __ LoadRoot(t1, Heap::kTheHoleValueRootIndex);
  // In case the loaded value is the_hole we have to consult GetProperty
  // to ensure the prototype chain is searched.
  __ Branch(&slow, eq, v0, Operand(t1));
  __ Ret();

  // Check whether the elements is a pixel array.
  // a0: key (untagged integer).
  // a1: elements array of the object.
  // a3: map of elements array.
  __ bind(&check_pixel_array);
  __ LoadRoot(t0, Heap::kPixelArrayMapRootIndex);
  __ Branch(&check_number_dictionary, ne, a3, Operand(t0));
  // Check that the key (index) is within bounds.
  __ lw(t0, FieldMemOperand(a1, PixelArray::kLengthOffset));
  __ Branch(&slow, hs, a0, Operand(t0));
  __ lw(t0, FieldMemOperand(a1, PixelArray::kExternalPointerOffset));
  __ Addu(t0, t0, Operand(a0));
  __ lbu(v0, MemOperand(t0, 0));
  __ sll(v0, v0, kSmiTagSize);  // Tag result as smi.
  __ Ret();

  __ bind(&check_number_dictionary);
  // Check whether the elements is a number dictionary.
  // a0: untagged index
  // a1: elements
  // a2: key
  // a3: map of elements array.
  __ LoadRoot(t0, Heap::kHashTableMapRootIndex);
  __ Branch(&slow, ne, a3, Operand(t0));

  GenerateNumberDictionaryLoad(masm, &slow, a1, a2, a0, a3, t0);
  __ mov(v0, a0);  // Return value in v0.
  __ Ret();

  // Slow case: Push extra copies of the arguments (2).
  __ bind(&slow);
  __ IncrementCounter(&Counters::keyed_load_generic_slow, 1, a0, a1);
  __ lw(a0, MemOperand(sp, 0));
  GenerateRuntimeGetProperty(masm);
}


void KeyedLoadIC::GenerateString(MacroAssembler* masm) {
  // ra     : return address
  // a0     : key
  // sp[0]  : key
  // sp[4]  : receiver

  Label miss;
  Label index_not_smi;
  Label index_out_of_range;
  Label slow_char_code;
  Label got_char_code;

  // Get the object from the stack.
  __ lw(a1, MemOperand(sp, kPointerSize));

  Register object = a1;
  Register index = a0;
  Register code = a2;
  Register scratch = a3;

  StringHelper::GenerateFastCharCodeAt(masm,
                                       object,
                                       index,
                                       scratch,
                                       code,
                                       &miss,  // When not a string.
                                       &index_not_smi,
                                       &index_out_of_range,
                                       &slow_char_code);

  // If we didn't bail out, code register contains smi tagged char
  // code.
  __ bind(&got_char_code);
  StringHelper::GenerateCharFromCode(masm, code, scratch, v0, JUMP_FUNCTION);
#ifdef DEBUG
  __ Abort("Unexpected fall-through from char from code tail call");
#endif

  // Check if key is a heap number.
  __ bind(&index_not_smi);
  __ CheckMap(index, scratch, Factory::heap_number_map(), &miss, true);

  // Push receiver and key on the stack (now that we know they are a
  // string and a number), and call runtime.
  __ bind(&slow_char_code);
  __ EnterInternalFrame();
  __ MultiPush(object.bit() | index.bit());
  __ CallRuntime(Runtime::kStringCharCodeAt, 2);
  ASSERT(!code.is(v0));
  __ mov(code, v0);
  __ LeaveInternalFrame();

  // Check if the runtime call returned NaN char code. If yes, return
  // undefined. Otherwise, we can continue.
  if (FLAG_debug_code) {
    __ BranchOnSmi(code, &got_char_code);
    __ lw(scratch, FieldMemOperand(code, HeapObject::kMapOffset));
    __ LoadRoot(at, Heap::kHeapNumberMapRootIndex);
    __ Assert(eq,
              "StringCharCodeAt must return smi or heap number",
              scratch,
              Operand(at));
  }
  __ LoadRoot(scratch, Heap::kNanValueRootIndex);
  __ Branch(&got_char_code, ne, code, Operand(scratch));
  __ bind(&index_out_of_range);
  __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
  __ Ret();

  __ bind(&miss);
  GenerateGeneric(masm);
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
  //  -- sp[0]  : key
  //  -- sp[4]  : receiver
  // -----------------------------------
  Label slow, failed_allocation;

  // Get the object from the stack.
  __ lw(a1, MemOperand(sp, kPointerSize));

  // a0: key
  // a1: receiver object

  // Check that the object isn't a smi
  __ BranchOnSmi(a1, &slow);

  // Check that the key is a smi.
  __ BranchOnNotSmi(a0, &slow);

  // Check that the object is a JS object. Load map into a2.
  __ GetObjectType(a1, a2, a3);
  __ Branch(&slow, lt, a3, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that the receiver does not require access checks.  We need
  // to check this explicitly since this generic stub does not perform
  // map checks.
  __ lbu(a3, FieldMemOperand(a2, Map::kBitFieldOffset));
  __ And(a3, a3, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&slow, ne, a3, Operand(zero_reg));

  // Check that the elements array is the appropriate type of
  // ExternalArray.
  // a0: index (as a smi)
  // a1: JSObject
  __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));
  __ lw(a2, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::RootIndexForExternalArrayType(array_type));
  __ Branch(&slow, ne, a2, Operand(t0));

  // Check that the index is in range.
  __ lw(t0, FieldMemOperand(a1, ExternalArray::kLengthOffset));
  __ sra(t1, a0, kSmiTagSize);
  // Unsigned comparison catches both negative and too-large values.
  __ Branch(&slow, Uless, t0, Operand(t1));

  // a0: index (smi)
  // t1: key (un-tagged)
  // a1: elements array
  __ lw(a1, FieldMemOperand(a1, ExternalArray::kExternalPointerOffset));
  // a1: base pointer of external storage

  switch (array_type) {
    case kExternalByteArray:
      __ addu(t0, a1, t1);
      __ lb(a0, MemOperand(t0, 0));
      break;
    case kExternalUnsignedByteArray:
      __ addu(t0, a1, t1);
      __ lbu(a0, MemOperand(t0, 0));
      break;
    case kExternalShortArray:
      __ sll(t0, t1, 1);
      __ addu(t0, a1, t0);
      __ lh(a0, MemOperand(t0, 0));
      break;
    case kExternalUnsignedShortArray:
      __ sll(t0, t1, 1);
      __ addu(t0, a1, t0);
      __ lhu(a0, MemOperand(t0, 0));
      break;
    case kExternalIntArray:
    case kExternalUnsignedIntArray:
      __ sll(t0, t1, 2);
      __ addu(t0, a1, t0);
      __ lw(a0, MemOperand(t0, 0));
      break;
    case kExternalFloatArray:
      if (CpuFeatures::IsSupported(FPU)) {
        CpuFeatures::Scope scope(FPU);
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
    __ Subu(t0, a0, Operand(0xc0000000));  // Non-smi value gives neg result.
    __ Branch(&box_int, lt, t0, Operand(zero_reg));
    __ sll(v0, a0, kSmiTagSize);
    __ Ret();

    __ bind(&box_int);

    // Allocate a HeapNumber for the int and perform int-to-double
    // conversion.
    __ AllocateHeapNumber(v0, a3, t0, &slow);

    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      __ mtc1(a0, f0);
      __ cvt_d_w(f0, f0);
      __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset - kHeapObjectTag));
      __ Ret();
    } else {
      WriteInt32ToHeapNumberStub stub(a0, v0, t0, t1);
      __ TailCallStub(&stub);
    }
  } else if (array_type == kExternalUnsignedIntArray) {
    // The test is different for unsigned int values. Since we need
    // the value to be in the range of a positive smi, we can't
    // handle either of the top two bits being set in the value.
    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      Label pl_box_int;
      __ And(t0, a0, Operand(0xC0000000));
      __ Branch(&pl_box_int, ne, t0, Operand(zero_reg));

      // It can fit in an Smi.
      __ sll(v0, a0, kSmiTagSize);
      __ Ret();

      __ bind(&pl_box_int);
      __ AllocateHeapNumber(v0, a1, a2, &slow);
      __ mtc1(a0, f0);        // LS 32-bits.
      __ mtc1(zero_reg, f1);  // MS 32-bits are all zero.
      __ cvt_d_l(f0, f0);     // Use 64 bit conv to get correct unsigned 32-bit.
      __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset - kHeapObjectTag));

      __ Ret();
    } else {
      // Check whether unsigned integer fits into smi.
      Label box_int_0, box_int_1, done;
      __ And(t0, a0, Operand(0x80000000));
      __ Branch(&box_int_0, ne, t0, Operand(zero_reg));
      __ And(t0, a0, Operand(0x40000000));
      __ Branch(&box_int_1, ne, t0, Operand(zero_reg));

      // Tag integer as smi and return it.
      __ sll(v0, a0, kSmiTagSize);
      __ Ret();

      __ bind(&box_int_0);
      // Integer does not have leading zeros.
      GenerateUInt2Double(masm, a0, a1, t0, 0);
      __ b(&done);

      __ bind(&box_int_1);
      // Integer has one leading zero.
      GenerateUInt2Double(masm, a0, a1, t0, 1);

      __ bind(&done);
      // Integer was converted to double in registers a0:a1.
      // Wrap it into a HeapNumber.
      __ AllocateHeapNumber(t2, t3, t5, &slow);

      __ sw(a0, FieldMemOperand(t2, HeapNumber::kExponentOffset));
      __ sw(a1, FieldMemOperand(t2, HeapNumber::kMantissaOffset));

      __ mov(v0, t2);
      __ Ret();
    }
  } else if (array_type == kExternalFloatArray) {
    // For the floating-point array type, we need to always allocate a
    // HeapNumber.
    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      __ AllocateHeapNumber(v0, a1, a2, &slow);
      // The float (single) value is already in fpu reg f0 (if we use float).
      __ cvt_d_s(f0, f0);
      __ sdc1(f0, MemOperand(v0, HeapNumber::kValueOffset - kHeapObjectTag));
      __ Ret();
    } else {
      __ AllocateHeapNumber(v0, t1, t2, &slow);
      // FPU is not available, do manual single to double conversion.

      // a0: floating point value (binary32).

      // Extract mantissa to a1.
      __ And(a1, a0, Operand(kBinary32MantissaMask));

      // Extract exponent to a2.
      __ srl(a2, a0, kBinary32MantissaBits);
      __ And(a2, a2, Operand(kBinary32ExponentMask >> kBinary32MantissaBits));

      Label exponent_rebiased;
      __ Branch(&exponent_rebiased, eq, a2, Operand(zero_reg));

      __ li(t0, 0x7ff);
      __ Xor(t1, a2, Operand(0xFF));
      __ movz(a2, t0, t1);  // Set a2 to 0x7ff only if to is equal to 0xff.
      __ Branch(&exponent_rebiased, eq, t0, Operand(0xff));

      // Rebias exponent.
      __ Addu(a2, a2, Operand(-kBinary32ExponentBias + HeapNumber::kExponentBias));

      __ bind(&exponent_rebiased);
      __ And(a0, a0, Operand(kBinary32SignMask));
      __ sll(a2, a2, HeapNumber::kMantissaBitsInTopWord);
      __ or_(a0, a0, a2);

      // Shift mantissa.
      static const int kMantissaShiftForHiWord =
          kBinary32MantissaBits - HeapNumber::kMantissaBitsInTopWord;

      static const int kMantissaShiftForLoWord =
          kBitsPerInt - kMantissaShiftForHiWord;

      __ srl(t0, a1, kMantissaShiftForHiWord);
      __ or_(a0, a0, t0);
      __ sll(a1, a1, kMantissaShiftForLoWord);

      __ sw(a0, FieldMemOperand(v0, HeapNumber::kExponentOffset));
      __ sw(a1, FieldMemOperand(v0, HeapNumber::kMantissaOffset));
      __ Ret();
    }

  } else {
    // Remaining array-types will all fit in Smi.
    __ sll(v0, a0, kSmiTagSize);
    __ Ret();
  }

  // Slow case: Load name and receiver from stack and jump to runtime.
  __ bind(&slow);
  __ IncrementCounter(&Counters::keyed_load_external_array_slow, 1, a0, a1);
  __ lw(a0, MemOperand(sp, 0));
  GenerateRuntimeGetProperty(masm);
}


void KeyedStoreIC::GenerateRuntimeSetProperty(MacroAssembler* masm) {
  // a0     : value
  // ra     : return address
  // sp[0]  : key
  // sp[1]  : receiver
  __ lw(a1, MemOperand(sp, 0));
  __ lw(a3, MemOperand(sp, 4));
  __ MultiPush(a0.bit() | a1.bit() | a3.bit());

  __ TailCallRuntime(Runtime::kSetProperty, 3, 1);
}


void KeyedStoreIC::GenerateGeneric(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- a0     : value
  //  -- ra     : return address
  //  -- sp[0]  : key
  //  -- sp[1]  : receiver
  // -----------------------------------
  Label slow, fast, array, extra, exit, check_pixel_array;

  // Get the key and receiver object from the stack (don't pop).
  __ lw(a1, MemOperand(sp, 0));  // a1 = key.
  __ lw(a3, MemOperand(sp, 4));  // a3 = receiver.
  // Check that the key is a smi.
  __ BranchOnNotSmi(a1, &slow);
  // Check that the object isn't a smi.
  __ BranchOnSmi(a3, &slow);

  // Get the map of the object.
  __ lw(a2, FieldMemOperand(a3, HeapObject::kMapOffset));
  // Check that the receiver does not require access checks.  We need
  // to do this because this generic stub does not perform map checks.
  __ lbu(t0, FieldMemOperand(a2, Map::kBitFieldOffset));
  __ And(t0, t0, Operand(1 << Map::kIsAccessCheckNeeded));
  __ Branch(&slow, ne, t0, Operand(zero_reg));
  // Check if the object is a JS array or not.
  __ lbu(a2, FieldMemOperand(a2, Map::kInstanceTypeOffset));
  // a1 == key.
  __ Branch(&array, eq, a2, Operand(JS_ARRAY_TYPE));
  // Check that the object is some kind of JS object.
  __ Branch(&slow, lt, a2, Operand(FIRST_JS_OBJECT_TYPE));

  // Object case: Check key against length in the elements array.
  __ lw(a3, FieldMemOperand(a3, JSObject::kElementsOffset));
  // Check that the object is in fast mode (not dictionary).
  __ lw(a2, FieldMemOperand(a3, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::kFixedArrayMapRootIndex);
  __ Branch(&check_pixel_array, ne, a2, Operand(t0));
  // Untag the key (for checking against untagged length in the fixed array).
  __ sra(a1, a1, kSmiTagSize);
  // Compute address to store into and check array bounds.
  __ Addu(a2, a3, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t0, a1, kPointerSizeLog2);  // Scale index for words.
  __ Addu(a2, a2, Operand(t0));  // Base + index.
  __ lw(t0, FieldMemOperand(a3, FixedArray::kLengthOffset));
  __ Branch(&fast, lo, a1, Operand(t0));
  // Fall thru to slow if un-tagged index >= length.

  // Slow case:
  __ bind(&slow);
  GenerateRuntimeSetProperty(masm);

  // Check whether the elements is a pixel array.
  // a0: value.
  // a1: index (as a smi), zero-extended.
  // a2: map of elements array.
  // a3: elements array.
  __ bind(&check_pixel_array);
  __ LoadRoot(t0, Heap::kPixelArrayMapRootIndex);
  __ Branch(&slow, ne, a2, Operand(t0));
  // Check that the value is a smi. If a conversion is needed call into the
  // runtime to convert and clamp.
  __ BranchOnNotSmi(a0, &slow);
  __ sra(a1, a1, kSmiTagSize);  // Untag the key.
  __ lw(t0, FieldMemOperand(a3, PixelArray::kLengthOffset));
  __ Branch(&slow, hs, a1, Operand(t0));
  __ mov(t0, a0);  // Save the value.
  __ sra(a0, a0, kSmiTagSize);  // Untag the value.
  {  // Clamp the value to [0..255].
    Label done;
    __ li(v0, Operand(255));
    __ Branch(&done, gt, a0, Operand(v0));  // Normal: nop in delay slot.
    __ Branch(false, &done, lt, a0, Operand(zero_reg));  // Use delay slot.
    __ mov(v0, zero_reg);  // In delay slot.
    __ mov(v0, a0);  // Value is in range 0..255.
    __ bind(&done);
  }
  __ lw(a2, FieldMemOperand(a3, PixelArray::kExternalPointerOffset));
  __ Addu(a0, a2, a1);  // Base + index.
  __ sb(v0, MemOperand(a0, 0));
  __ mov(v0, t0);  // Return the original value.
  __ Ret();


  // Extra capacity case: Check if there is extra capacity to
  // perform the store and update the length. Used for adding one
  // element to the array by writing to array[array.length].
  // a0 == value, a1 == key, a2 == elements, a3 == object
  // t0 == current array len
  __ bind(&extra);
  // Do not leave holes in the array.
  __ Branch(&slow, ne, a1, Operand(t0));
  // Check for room in the elements backing store.
  __ sra(a1, a1, kSmiTagSize);  // Untag key.
  __ lw(t0, FieldMemOperand(a2, Array::kLengthOffset));
  __ Branch(&slow, hs, a1, Operand(t0));
  __ sll(a1, a1, kSmiTagSize);  // Restore key tag.
  __ Addu(a1, a1, Operand(Smi::FromInt(1)));  // Increment key as Smi.
  __ sw(a1, FieldMemOperand(a3, JSArray::kLengthOffset));  // New length.
  __ mov(a3, a2);
  // NOTE: Computing the address to store into must take the fact
  // that the key has been incremented into account.
  int displacement = FixedArray::kHeaderSize - kHeapObjectTag -
      ((1 << kSmiTagSize) * 2);
  __ Addu(a2, a2, Operand(displacement));
  __ sll(t0, a1, kPointerSizeLog2 - kSmiTagSize); // Scale smi to word index.
  __ Addu(a2, a2, Operand(t0));  // Base + index.
  __ Branch(&fast, al);


  // Array case: Get the length and the elements array from the JS
  // array. Check that the array is in fast mode; if it is the
  // length is always a smi.
  // a0 == value, a3 == object
  __ bind(&array);
  __ lw(a2, FieldMemOperand(a3, JSObject::kElementsOffset));
  __ lw(a1, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ LoadRoot(t0, Heap::kFixedArrayMapRootIndex);
  __ Branch(&slow, ne, a1, Operand(t0));

  // Check the key against the length in the array, compute the
  // address to store into and fall through to fast case.
  __ lw(a1, MemOperand(sp)); // Restore key
  // a0 == value, a1 == key, a2 == elements, a3 == object.
  __ lw(t0, FieldMemOperand(a3, JSArray::kLengthOffset));
  __ Branch(&extra, hs, a1, Operand(t0));
  __ mov(a3, a2);
  __ Addu(a2, a2, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t0, a1, kPointerSizeLog2 - kSmiTagSize); // Scale smi to word index.
  __ Addu(a2, a2, Operand(t0));  // Base + index


  // Fast case: Do the store.
  // a0 == value, a2 == address to store into, a3 == elements
  __ bind(&fast);
  __ sw(a0, MemOperand(a2, 0));
  // Skip write barrier if the written value is a smi.
  __ BranchOnSmi(a0, &exit);
  // Update write barrier for the elements array address.
  __ Subu(a1, a2, Operand(a3));
  __ RecordWrite(a3, a1, a2);

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
  //  -- ra     : return address
  //  -- sp[0]  : key
  //  -- sp[1]  : receiver
  // -----------------------------------
  Label slow, check_heap_number;

  // Get the key and the object from the stack (don't pop).
  __ lw(a1, MemOperand(sp, 0));  // a1 = key.
  __ lw(a2, MemOperand(sp, 4));  // a2 = receiver.

  // Check that the object isn't a smi.
  __ BranchOnSmi(a2, &slow);

  // Check that the object is a JS object. Load map into a3.
  __ GetObjectType(a2, a3, t0);
  __ Branch(&slow, lt, t0, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that the receiver does not require access checks.  We need
  // to do this because this generic stub does not perform map checks.
  __ lbu(t1, FieldMemOperand(a3, Map::kBitFieldOffset));
  __ And(t1, t1, Operand(1 << Map::kIsAccessCheckNeeded));
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
  __ Branch(&slow, ne, a3, Operand(t1));

  // Check that the index is in range.
  __ sra(a1, a1, kSmiTagSize);  // Untag the index.
  __ lw(t1, FieldMemOperand(a2, ExternalArray::kLengthOffset));
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
      __ addu(t0, a2, a1);
      __ sb(a3, MemOperand(t0, 0));
      break;
    case kExternalShortArray:
    case kExternalUnsignedShortArray:
      __ sll(t0, a1, 1);
      __ addu(t0, a2, t0);
      __ sh(a3, MemOperand(t0, 0));
      break;
    case kExternalIntArray:
    case kExternalUnsignedIntArray:
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
  __ GetObjectType(a0, a3, t0);
  __ Branch(&slow, ne, t0, Operand(HEAP_NUMBER_TYPE));

  __ lw(a2, FieldMemOperand(a2, ExternalArray::kExternalPointerOffset));

  // The WebGL specification leaves the behavior of storing NaN and
  // +/-Infinity into integer arrays basically undefined. For more
  // reproducible behavior, convert these to zero.

  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);

    __ ldc1(f0, MemOperand(a0, HeapNumber::kValueOffset - kHeapObjectTag));

    if (array_type == kExternalFloatArray) {
      __ cvt_s_d(f0, f0);
      __ sll(t0, a1, 2);
      __ addu(t0, a2, t0);
      __ swc1(f0, MemOperand(t0, 0));
    } else {
      Label done;

      // Need to perform float-to-int conversion.
      // Test whether exponent equal to 0x7FF (infinity or NaN).

      __ mfc1(a3, f1);  // Move exponent word of double to a3 (as raw bits).
      __ li(t0, Operand(0x7FF00000));
      __ And(a3, a3, Operand(t0));
      __ Branch(false, &done, eq, a3, Operand(t0));
      __ mov(a3, zero_reg);  // In delay slot.

      // Not infinity or NaN simply convert to int.
      if (IsElementTypeSigned(array_type)) {
        __ trunc_w_d(f0, f0);
        __ mfc1(a3, f0);
      } else {
        __ trunc_l_d(f0, f0);  // Convert double to 64-bit int.
        __ mfc1(a3, f0);  // Keep the LS 32-bits.
      }

      // a1: index (integer)
      // a2: external array base address
      // a3: HeapNumber converted to integer
      __ bind(&done);
      switch (array_type) {
        case kExternalByteArray:
        case kExternalUnsignedByteArray:
          __ addu(t0, a2, a1);
          __ sb(a3, MemOperand(t0, 0));
          break;
        case kExternalShortArray:
        case kExternalUnsignedShortArray:
          __ sll(t0, a1, 1);
          __ addu(t0, a2, t0);
          __ sh(a3, MemOperand(t0, 0));
          break;
        case kExternalIntArray:
        case kExternalUnsignedIntArray:
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

    __ lw(t3, FieldMemOperand(a0, HeapNumber::kExponentOffset));
    __ lw(t4, FieldMemOperand(a0, HeapNumber::kMantissaOffset));

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
      __ Addu(t6, t6, Operand(kBinary32ExponentBias - HeapNumber::kExponentBias));

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
      __ sll(t0, a1, 2);
      __ addu(t0, a2, t0);
      __ sw(t3, MemOperand(t0, 0));
      __ mov(v0, a0);
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

      __ li(t0, HeapNumber::kMantissaBitsInTopWord);
      __ subu(t6, t0, t6);
      __ slt(t1, t6, zero_reg);
      __ srlv(t2, t3, t6);
      __ movz(t3, t2, t1);  // Only if t6 is positive.
      __ Branch(&sign, ge, t6, Operand(zero_reg));

      __ subu(t6, zero_reg, t6);
      __ sllv(t3, t3, t6);
      __ li(t0, meaningfull_bits);
      __ subu(t6, t0, t6);
      __ srlv(t4, t4, t6);
      __ or_(t3, t3, t4);

      __ bind(&sign);
      __ subu(t2, t3, zero_reg);
      __ movz(t3, t2, t5);  // Only if t5 is zero.

      __ bind(&done);
      switch (array_type) {
        case kExternalByteArray:
        case kExternalUnsignedByteArray:
          __ addu(t0, a2, a1);
          __ sb(t3, MemOperand(t0, 0));
          break;
        case kExternalShortArray:
        case kExternalUnsignedShortArray:
          __ sll(t0, a1, 1);
          __ addu(t0, a2, t0);
          __ sh(t3, MemOperand(t0, 0));
          break;
        case kExternalIntArray:
        case kExternalUnsignedIntArray:
          __ sll(t0, a1, 2);
          __ addu(t0, a2, t0);
          __ sw(t3, MemOperand(t0, 0));
          break;
        default:
          UNREACHABLE();
          break;
      }
    }
  }

  // Slow case: call runtime.
  __ bind(&slow);
  GenerateRuntimeSetProperty(masm);
}


void KeyedLoadIC::GenerateIndexedInterceptor(MacroAssembler* masm) {
  // ---------- S t a t e --------------
  //  -- ra     : return address
  //  -- a0     : key
  //  -- sp[0]  : key
  //  -- sp[4]  : receiver
  // -----------------------------------
  Label slow;

  // Get the object from the stack.
  __ lw(a1, MemOperand(sp, kPointerSize));

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

#endif  // V8_TARGET_ARCH_MIPS
