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

#include "ic-inl.h"
#include "codegen-inl.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm)


static void ProbeTable(MacroAssembler* masm,
                       Code::Flags flags,
                       StubCache::Table table,
                       Register name,
                       Register offset) {
  ExternalReference key_offset(SCTableReference::keyReference(table));
  ExternalReference value_offset(SCTableReference::valueReference(table));

  Label miss;

  // Save the offset on the stack.
  __ Push(offset);

  // Check that the key in the entry matches the name.
  __ li(t8, Operand(key_offset));
  __ sll(t9, offset, 1);
  __ addu(t9, t8, t9);
  __ lw(t8, MemOperand(t9));
  __ Branch(&miss, ne, name, Operand(t8));

  // Get the code entry from the cache.
  __ li(t8, Operand(value_offset));
  __ sll(t9, offset, 1);
  __ addu(t9, t8, t9);
  __ lw(offset, MemOperand(t9));

  // Check that the flags match what we're looking for.
  __ lw(offset, FieldMemOperand(offset, Code::kFlagsOffset));
  __ And(offset, offset, Operand(~Code::kFlagsNotUsedInLookup));
  __ Branch(&miss, ne, offset, Operand(flags));

  // Restore offset and re-load code entry from cache.
  __ Pop(offset);
  __ li(t8, Operand(value_offset));
  __ sll(t9, offset, 1);
  __ addu(t9, t8, t9);
  __ lw(offset, MemOperand(t9));

  // Jump to the first instruction in the code stub.
  __ Addu(offset, offset, Operand(Code::kHeaderSize - kHeapObjectTag));
  __ Jump(offset);

  // Miss: Restore offset and fall through.
  __ bind(&miss);
  __ Pop(offset);
}


void StubCache::GenerateProbe(MacroAssembler* masm,
                              Code::Flags flags,
                              Register receiver,
                              Register name,
                              Register scratch,
                              Register extra) {
  Label miss;

  // Make sure that code is valid. The shifting code relies on the
  // entry size being 8.
  ASSERT(sizeof(Entry) == 8);

  // Make sure the flags does not name a specific type.
  ASSERT(Code::ExtractTypeFromFlags(flags) == 0);

  // Make sure that there are no register conflicts.
  ASSERT(!scratch.is(receiver));
  ASSERT(!scratch.is(name));

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver, &miss, t0);

  // Get the map of the receiver and compute the hash.
  __ lw(scratch, FieldMemOperand(name, String::kHashFieldOffset));
  __ lw(t8, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ Addu(scratch, scratch, Operand(t8));
  __ Xor(scratch, scratch, Operand(flags));
  __ And(scratch,
         scratch,
         Operand((kPrimaryTableSize - 1) << kHeapObjectTagSize));

  // Probe the primary table.
  ProbeTable(masm, flags, kPrimary, name, scratch);

  // Primary miss: Compute hash for secondary probe.
  __ Subu(scratch, scratch, Operand(name));
  __ Addu(scratch, scratch, Operand(flags));
  __ And(scratch,
         scratch,
         Operand((kSecondaryTableSize - 1) << kHeapObjectTagSize));

  // Probe the secondary table.
  ProbeTable(masm, flags, kSecondary, name, scratch);

  // Cache miss: Fall-through and let caller handle the miss by
  // entering the runtime system.
  __ bind(&miss);
}


void StubCompiler::GenerateLoadGlobalFunctionPrototype(MacroAssembler* masm,
                                                       int index,
                                                       Register prototype) {
  // Load the global or builtins object from the current context.
  __ lw(prototype, MemOperand(cp, Context::SlotOffset(Context::GLOBAL_INDEX)));
  // Load the global context from the global or builtins object.
  __ lw(prototype,
         FieldMemOperand(prototype, GlobalObject::kGlobalContextOffset));
  // Load the function from the global context.
  __ lw(prototype, MemOperand(prototype, Context::SlotOffset(index)));
  // Load the initial map.  The global functions all have initial maps.
  __ lw(prototype,
         FieldMemOperand(prototype, JSFunction::kPrototypeOrInitialMapOffset));
  // Load the prototype from the initial map.
  __ lw(prototype, FieldMemOperand(prototype, Map::kPrototypeOffset));
}


// Load a fast property out of a holder object (src). In-object properties
// are loaded directly otherwise the property is loaded from the properties
// fixed array.
void StubCompiler::GenerateFastPropertyLoad(MacroAssembler* masm,
                                            Register dst, Register src,
                                            JSObject* holder, int index) {
  // Adjust for the number of properties stored in the holder.
  index -= holder->map()->inobject_properties();
  if (index < 0) {
    // Get the property straight out of the holder.
    int offset = holder->map()->instance_size() + (index * kPointerSize);
    __ lw(dst, FieldMemOperand(src, offset));
  } else {
    // Calculate the offset into the properties array.
    int offset = index * kPointerSize + FixedArray::kHeaderSize;
    __ lw(dst, FieldMemOperand(src, JSObject::kPropertiesOffset));
    __ lw(dst, FieldMemOperand(dst, offset));
  }
}


void StubCompiler::GenerateLoadArrayLength(MacroAssembler* masm,
                                           Register receiver,
                                           Register scratch,
                                           Label* miss_label) {
  // Check that the receiver isn't a smi.
  __ And(scratch, receiver, Operand(kSmiTagMask));
  __ Branch(miss_label, eq, scratch, Operand(zero_reg));

  // Check that the object is a JS array.
  __ GetObjectType(receiver, scratch, scratch);
  __ Branch(miss_label, ne, scratch, Operand(JS_ARRAY_TYPE));

  // Load length directly from the JS array.
  __ lw(v0, FieldMemOperand(receiver, JSArray::kLengthOffset));
  __ Ret();
}


// Generate code to check if an object is a string.  If the object is a
// heap object, its map's instance type is left in the scratch1 register.
// If this is not needed, scratch1 and scratch2 may be the same register.
static void GenerateStringCheck(MacroAssembler* masm,
                                Register receiver,
                                Register scratch1,
                                Register scratch2,
                                Label* smi,
                                Label* non_string_object) {
  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver, smi, t0);

  // Check that the object is a string.
  __ lw(scratch1, FieldMemOperand(receiver, HeapObject::kMapOffset));
  __ lbu(scratch1, FieldMemOperand(scratch1, Map::kInstanceTypeOffset));
  __ And(scratch2, scratch1, Operand(kIsNotStringMask));
  // The cast is to resolve the overload for the argument of 0x0.
  __ Branch(non_string_object,
            ne,
            scratch2,
            Operand(static_cast<int32_t>(kStringTag)));
}


// Generate code to load the length from a string object and return the length.
// If the receiver object is not a string or a wrapped string object the
// execution continues at the miss label. The register containing the
// receiver is potentially clobbered.
void StubCompiler::GenerateLoadStringLength(MacroAssembler* masm,
                                            Register receiver,
                                            Register scratch1,
                                            Register scratch2,
                                            Label* miss) {
  Label check_wrapper;

  // Check if the object is a string leaving the instance type in the
  // scratch1 register.
  GenerateStringCheck(masm, receiver, scratch1, scratch2, miss, &check_wrapper);

  // Load length directly from the string.
  __ lw(v0, FieldMemOperand(receiver, String::kLengthOffset));
  __ sll(v0, v0, kSmiTagSize);
  __ Ret();

  // Check if the object is a JSValue wrapper.
  __ bind(&check_wrapper);
  __ Branch(miss, ne, scratch1, Operand(JS_VALUE_TYPE));

  // Unwrap the value and check if the wrapped value is a string.
  __ lw(scratch1, FieldMemOperand(receiver, JSValue::kValueOffset));
  GenerateStringCheck(masm, scratch1, scratch2, scratch2, miss, miss);
  __ lw(v0, FieldMemOperand(scratch1, String::kLengthOffset));
  __ sll(v0, v0, kSmiTagSize);
  __ Ret();
}


void StubCompiler::GenerateLoadFunctionPrototype(MacroAssembler* masm,
                                                 Register receiver,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 Label* miss_label) {
  __ TryGetFunctionPrototype(receiver, scratch1, scratch2, miss_label);
  __ mov(v0, scratch1);
  __ Ret();
}


// Generate StoreField code, value is passed in a0 register.
// After executing generated code, the receiver_reg and name_reg
// may be clobbered.
void StubCompiler::GenerateStoreField(MacroAssembler* masm,
                                      JSObject* object,
                                      int index,
                                      Map* transition,
                                      Register receiver_reg,
                                      Register name_reg,
                                      Register scratch,
                                      Label* miss_label) {
  // a0 : value
  Label exit;

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver_reg, miss_label, scratch);

  // Check that the map of the receiver hasn't changed.
  __ lw(scratch, FieldMemOperand(receiver_reg, HeapObject::kMapOffset));
  __ Branch(miss_label, ne, scratch, Operand(Handle<Map>(object->map())));

  // Perform global security token check if needed.
  if (object->IsJSGlobalProxy()) {
    __ CheckAccessGlobalProxy(receiver_reg, scratch, miss_label);
  }

  // Stub never generated for non-global objects that require access
  // checks.
  ASSERT(object->IsJSGlobalProxy() || !object->IsAccessCheckNeeded());

  // Perform map transition for the receiver if necessary.
  if ((transition != NULL) && (object->map()->unused_property_fields() == 0)) {
    // The properties must be extended before we can store the value.
    // We jump to a runtime call that extends the properties array.
    __ Push(receiver_reg);
    __ li(a2, Operand(Handle<Map>(transition)));
    __ MultiPush(a2.bit() | a0.bit());
    __ TailCallExternalReference(
           ExternalReference(IC_Utility(IC::kSharedStoreIC_ExtendStorage)),
           3, 1);
    return;
  }

  if (transition != NULL) {
    // Update the map of the object; no write barrier updating is
    // needed because the map is never in new space.
    __ li(t0, Operand(Handle<Map>(transition)));
    __ sw(t0, FieldMemOperand(receiver_reg, HeapObject::kMapOffset));
  }

  // Adjust for the number of properties stored in the object. Even in the
  // face of a transition we can use the old map here because the size of the
  // object and the number of in-object properties is not going to change.
  index -= object->map()->inobject_properties();

  if (index < 0) {
    // Set the property straight into the object.
    int offset = object->map()->instance_size() + (index * kPointerSize);
    __ sw(a0, FieldMemOperand(receiver_reg, offset));

    // Skip updating write barrier if storing a smi.
    __ BranchOnSmi(a0, &exit, scratch);

    // Update the write barrier for the array address.
    // Pass the value being stored in the now unused name_reg.
    __ li(name_reg, Operand(offset));
    __ RecordWrite(receiver_reg, name_reg, scratch);
  } else {
    // Write to the properties array.
    int offset = index * kPointerSize + FixedArray::kHeaderSize;
    // Get the properties array
    __ lw(scratch, FieldMemOperand(receiver_reg, JSObject::kPropertiesOffset));
    __ sw(a0, FieldMemOperand(scratch, offset));

    // Skip updating write barrier if storing a smi.
    __ BranchOnSmi(a0, &exit, scratch);

    // Update the write barrier for the array address.
    // Ok to clobber receiver_reg and name_reg, since we return.
    __ li(name_reg, Operand(offset));
    __ RecordWrite(scratch, name_reg, receiver_reg);
  }

  // Return the value (register v0).
  __ bind(&exit);
  __ mov(v0, a0);
  __ Ret();
}


void StubCompiler::GenerateLoadMiss(MacroAssembler* masm, Code::Kind kind) {
  ASSERT(kind == Code::LOAD_IC || kind == Code::KEYED_LOAD_IC);
  Code* code = NULL;
  if (kind == Code::LOAD_IC) {
    code = Builtins::builtin(Builtins::LoadIC_Miss);
  } else {
    code = Builtins::builtin(Builtins::KeyedLoadIC_Miss);
  }

  Handle<Code> ic(code);
  __ JumpToBuiltin(ic, RelocInfo::CODE_TARGET);
}


static void GenerateCallFunction(MacroAssembler* masm,
                                 Object* object,
                                 const ParameterCount& arguments,
                                 Label* miss) {
  // a0: receiver
  // a1: function to call

  // Check that the function really is a function.
  __ BranchOnSmi(a1, miss);
  __ GetObjectType(a1, a3, a3);
  __ Branch(miss, ne, a3, Operand(JS_FUNCTION_TYPE));

  // Patch the receiver on the stack with the global proxy if
  // necessary.
  if (object->IsGlobalObject()) {
    __ lw(a3, FieldMemOperand(a0, GlobalObject::kGlobalReceiverOffset));
    __ sw(a3, MemOperand(sp, arguments.immediate() * kPointerSize));
  }

  // Invoke the function.
  __ InvokeFunction(a1, arguments, JUMP_FUNCTION);
}


#undef __
#define __ ACCESS_MASM(masm())


Register StubCompiler::CheckPrototypes(JSObject* object,
                                       Register object_reg,
                                       JSObject* holder,
                                       Register holder_reg,
                                       Register scratch,
                                       String* name,
                                       int save_at_depth,
                                       Label* miss) {
  // TODO(602): support object saving.
  ASSERT(save_at_depth == kInvalidProtoDepth);

  // Check that the maps haven't changed.
  Register result =
      masm()->CheckMaps(object, object_reg, holder, holder_reg, scratch, miss);

  // If we've skipped any global objects, it's not enough to verify
  // that their maps haven't changed.
  while (object != holder) {
    if (object->IsGlobalObject()) {
      GlobalObject* global = GlobalObject::cast(object);
      Object* probe = global->EnsurePropertyCell(name);
      if (probe->IsFailure()) {
        set_failure(Failure::cast(probe));
        return result;
      }
      JSGlobalPropertyCell* cell = JSGlobalPropertyCell::cast(probe);
      ASSERT(cell->value()->IsTheHole());
      __ li(scratch, Operand(Handle<Object>(cell)));
      __ lw(scratch,
             FieldMemOperand(scratch, JSGlobalPropertyCell::kValueOffset));
      __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
      __ Branch(miss, ne, scratch, Operand(at));
    }
    object = JSObject::cast(object->GetPrototype());
  }

  // Return the register containing the holder.
  return result;
}


void StubCompiler::GenerateLoadField(JSObject* object,
                                     JSObject* holder,
                                     Register receiver,
                                     Register scratch1,
                                     Register scratch2,
                                     int index,
                                     String* name,
                                     Label* miss) {
  // Check that the receiver isn't a smi.
  __ And(scratch1, receiver, Operand(kSmiTagMask));
  __ Branch(miss, eq, scratch1, Operand(zero_reg));

  // Check that the maps haven't changed.
  Register reg =
      CheckPrototypes(object, receiver, holder, scratch1, scratch2, name, miss);
  GenerateFastPropertyLoad(masm(), v0, reg, holder, index);
  __ Ret();
}


void StubCompiler::GenerateLoadConstant(JSObject* object,
                                        JSObject* holder,
                                        Register receiver,
                                        Register scratch1,
                                        Register scratch2,
                                        Object* value,
                                        String* name,
                                        Label* miss) {
  // Check that the receiver isn't a smi.
  __ BranchOnSmi(receiver, miss, scratch1);

  // Check that the maps haven't changed.
  Register reg =
      CheckPrototypes(object, receiver, holder, scratch1, scratch2, name, miss);

  // Return the constant value.
  __ li(v0, Operand(Handle<Object>(value)));
  __ Ret();
}


bool StubCompiler::GenerateLoadCallback(JSObject* object,
                                        JSObject* holder,
                                        Register receiver,
                                        Register name_reg,
                                        Register scratch1,
                                        Register scratch2,
                                        AccessorInfo* callback,
                                        String* name,
                                        Label* miss,
                                        Failure** failure) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return false;   // UNIMPLEMENTED RETURN
}


void StubCompiler::GenerateLoadInterceptor(JSObject* object,
                                           JSObject* holder,
                                           LookupResult* lookup,
                                           Register receiver,
                                           Register name_reg,
                                           Register scratch1,
                                           Register scratch2,
                                           String* name,
                                           Label* miss) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
}


Object* StubCompiler::CompileLazyCompile(Code::Flags flags) {
  // Registers:
  // a1: function
  // ra: return address

  // Enter an internal frame.
  __ EnterInternalFrame();
  // Preserve the function.
  __ Push(a1);
  // Setup aligned call.
  __ SetupAlignedCall(t0, 1);
  // Push the function on the stack as the argument to the runtime function.
  __ Push(a1);
  // Call the runtime function
  __ CallRuntime(Runtime::kLazyCompile, 1);
  __ ReturnFromAlignedCall();
  // Calculate the entry point.
  __ addiu(t9, v0, Code::kHeaderSize - kHeapObjectTag);
  // Restore saved function.
  __ Pop(a1);
  // Tear down temporary frame.
  __ LeaveInternalFrame();
  // Do a tail-call of the compiled function.
  __ Jump(t9);

  return GetCodeWithFlags(flags, "LazyCompileStub");
}


Object* CallStubCompiler::CompileCallField(JSObject* object,
                                           JSObject* holder,
                                           int index,
                                           String* name) {
  // a2    : name
  // ra    : return address
  Label miss;

  const int argc = arguments().immediate();

  // Get the receiver of the function from the stack into r0.
  __ lw(a0, MemOperand(sp, argc * kPointerSize));
  // Check that the receiver isn't a smi.
  __ BranchOnSmi(a0, &miss, t0);

  // Do the right check and compute the holder register.
  Register reg = CheckPrototypes(object, a0, holder, a1, a3, name, &miss);
  GenerateFastPropertyLoad(masm(), a1, reg, holder, index);

  GenerateCallFunction(masm(), object, arguments(), &miss);

  // Handle call cache miss.
  __ bind(&miss);
  Handle<Code> ic = ComputeCallMiss(arguments().immediate());
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(FIELD, name);
}


Object* CallStubCompiler::CompileArrayPushCall(Object* object,
                                               JSObject* holder,
                                               JSFunction* function,
                                               String* name,
                                               CheckType check) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  // -----------------------------------

  // TODO(639): faster implementation.
  ASSERT(check == RECEIVER_MAP_CHECK);

  Label miss;

  // Get the receiver from the stack
  const int argc = arguments().immediate();
  __ lw(a1, MemOperand(sp, argc * kPointerSize));

  // Check that the receiver isn't a smi.
  __ And(at, a1, Operand(kSmiTagMask));
  __ Branch(&miss, eq, at, Operand(zero_reg));

  // Check that the maps haven't changed.
  CheckPrototypes(JSObject::cast(object), a1, holder, a3, a0, name, &miss);

  if (object->IsGlobalObject()) {
    __ lw(a3, FieldMemOperand(a1, GlobalObject::kGlobalReceiverOffset));
    __ sw(a3, MemOperand(sp, argc * kPointerSize));
  }

  __ TailCallExternalReference(ExternalReference(Builtins::c_ArrayPush),
                               argc + 1,
                               1);

  // Handle call cache miss.
  __ bind(&miss);
  Handle<Code> ic = ComputeCallMiss(arguments().immediate());
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  String* function_name = NULL;
  if (function->shared()->name()->IsString()) {
    function_name = String::cast(function->shared()->name());
  }
  return GetCode(CONSTANT_FUNCTION, function_name);
}


Object* CallStubCompiler::CompileArrayPopCall(Object* object,
                                              JSObject* holder,
                                              JSFunction* function,
                                              String* name,
                                              CheckType check) {
  // a2    : name
  // ra    : return address

  // TODO(642): faster implementation.
  ASSERT(check == RECEIVER_MAP_CHECK);

  Label miss;

  // Get the receiver from the stack
  const int argc = arguments().immediate();
  __ lw(a1, MemOperand(sp, argc * kPointerSize));

  // Check that the receiver isn't a smi.
  __ BranchOnSmi(a1, &miss, t1);

  // Check that the maps haven't changed.
  CheckPrototypes(JSObject::cast(object), a1, holder, a3, a0, name, &miss);

  if (object->IsGlobalObject()) {
    __ lw(a3, FieldMemOperand(a1, GlobalObject::kGlobalReceiverOffset));
    __ sw(a3, MemOperand(sp, argc * kPointerSize));
  }

  __ TailCallExternalReference(ExternalReference(Builtins::c_ArrayPop),
                               argc + 1,
                               1);

  // Handle call cache miss.
  __ bind(&miss);
  Handle<Code> ic = ComputeCallMiss(arguments().immediate());
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  String* function_name = NULL;
  if (function->shared()->name()->IsString()) {
    function_name = String::cast(function->shared()->name());
  }
  return GetCode(CONSTANT_FUNCTION, function_name);
}


Object* CallStubCompiler::CompileCallConstant(Object* object,
                                              JSObject* holder,
                                              JSFunction* function,
                                              String* name,
                                              CheckType check) {
  // a2: name
  // ra: return address
  SharedFunctionInfo* function_info = function->shared();
  if (function_info->HasCustomCallGenerator()) {
    CustomCallGenerator generator =
        ToCData<CustomCallGenerator>(function_info->function_data());
    return generator(this, object, holder, function, name, check);
  }

  Label miss;

  // Get the receiver from the stack
  const int argc = arguments().immediate();
  __ lw(a1, MemOperand(sp, argc * kPointerSize));

  // Check that the receiver isn't a smi.
  if (check != NUMBER_CHECK) {
    __ And(t1, a1, Operand(kSmiTagMask));
    __ Branch(&miss, eq, t1, Operand(zero_reg));
  }

  // Make sure that it's okay not to patch the on stack receiver
  // unless we're doing a receiver map check.
  ASSERT(!object->IsGlobalObject() || check == RECEIVER_MAP_CHECK);

  switch (check) {
    case RECEIVER_MAP_CHECK:
      // Check that the maps haven't changed.
      CheckPrototypes(JSObject::cast(object), a1, holder, a3, a0, name, &miss);

      // Patch the receiver on the stack with the global proxy if
      // necessary.
      if (object->IsGlobalObject()) {
        __ lw(a3, FieldMemOperand(a1, GlobalObject::kGlobalReceiverOffset));
        __ sw(a3, MemOperand(sp, argc * kPointerSize));
      }
      break;

    case STRING_CHECK:
      // __ break_(__LINE__);
      if (!function->IsBuiltin()) {
        // Calling non-builtins with a value as receiver requires boxing.
        __ jmp(&miss);
      } else {
        // Check that the object is a two-byte string or a symbol.
        __ GetObjectType(a1, a3, a3);
        __ Branch(&miss, Ugreater_equal, a3, Operand(FIRST_NONSTRING_TYPE));
        // Check that the maps starting from the prototype haven't changed.
        GenerateLoadGlobalFunctionPrototype(masm(),
                                            Context::STRING_FUNCTION_INDEX,
                                            a0);
        CheckPrototypes(JSObject::cast(object->GetPrototype()), a0, holder, a3,
                        a1, name, &miss);
      }
      break;

    case NUMBER_CHECK: {
      // __ break_(__LINE__);
      if (!function->IsBuiltin()) {
        // Calling non-builtins with a value as receiver requires boxing.
        __ jmp(&miss);
      } else {
      Label fast;
        // Check that the object is a smi or a heap number.
        __ And(t1, a1, Operand(kSmiTagMask));
        __ Branch(&fast, eq, t1, Operand(zero_reg));
        __ GetObjectType(a1, a0, a0);
        __ Branch(&miss, ne, a0, Operand(HEAP_NUMBER_TYPE));
        __ bind(&fast);
        // Check that the maps starting from the prototype haven't changed.
        GenerateLoadGlobalFunctionPrototype(masm(),
                                            Context::NUMBER_FUNCTION_INDEX,
                                            a0);
        CheckPrototypes(JSObject::cast(object->GetPrototype()), a0, holder, a3,
                        a1, name, &miss);
      }
      break;
    }

    case BOOLEAN_CHECK: {
      // __ break_(__LINE__);
      if (!function->IsBuiltin()) {
        // Calling non-builtins with a value as receiver requires boxing.
        __ jmp(&miss);
      } else {
        Label fast;
        // Check that the object is a boolean.
        __ LoadRoot(t0, Heap::kTrueValueRootIndex);
        __ Branch(&fast, eq, a1, Operand(t0));
        __ LoadRoot(t0, Heap::kFalseValueRootIndex);
        __ Branch(&miss, ne, a1, Operand(t0));
        __ bind(&fast);
        // Check that the maps starting from the prototype haven't changed.
        GenerateLoadGlobalFunctionPrototype(masm(),
                                            Context::BOOLEAN_FUNCTION_INDEX,
                                            a0);
        CheckPrototypes(JSObject::cast(object->GetPrototype()), a0, holder, a3,
                        a1, name, &miss);
      }
      break;
    }

    default:
      UNREACHABLE();
  }

  __ InvokeFunction(function, arguments(), JUMP_FUNCTION);

  // Handle call cache miss.
  __ bind(&miss);
  Handle<Code> ic = ComputeCallMiss(arguments().immediate());
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  String* function_name = NULL;
  if (function->shared()->name()->IsString()) {
    function_name = String::cast(function->shared()->name());
  }
  return GetCode(CONSTANT_FUNCTION, function_name);
}


Object* CallStubCompiler::CompileCallInterceptor(JSObject* object,
                                                 JSObject* holder,
                                                 String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return GetCode(INTERCEPTOR, name);
}


Object* CallStubCompiler::CompileCallGlobal(JSObject* object,
                                            GlobalObject* holder,
                                            JSGlobalPropertyCell* cell,
                                            JSFunction* function,
                                            String* name) {
  // r2    : name
  // ra: return address
  Label miss;

  // Get the number of arguments.
  const int argc = arguments().immediate();

  // Get the receiver from the stack.
  __ lw(a0, MemOperand(sp, argc * kPointerSize));

  // If the object is the holder then we know that it's a global
  // object which can only happen for contextual calls. In this case,
  // the receiver cannot be a smi.
  if (object != holder) {
    __ And(t0, a0, Operand(kSmiTagMask));
    __ Branch(&miss, eq, t0, Operand(zero_reg));
  }

  // Check that the maps haven't changed.
  CheckPrototypes(object, a0, holder, a3, a1, name, &miss);

  // Get the value from the cell.
  __ li(a3, Operand(Handle<JSGlobalPropertyCell>(cell)));
  __ lw(a1, FieldMemOperand(a3, JSGlobalPropertyCell::kValueOffset));

  // Check that the cell contains the same function.
  if (Heap::InNewSpace(function)) {
    // We can't embed a pointer to a function in new space so we have
    // to verify that the shared function info is unchanged. This has
    // the nice side effect that multiple closures based on the same
    // function can all use this call IC. Before we load through the
    // function, we have to verify that it still is a function.
    __ BranchOnSmi(a1, &miss, t0);
    __ GetObjectType(a1, a3, a3);
    __ Branch(&miss, ne, a3, Operand(JS_FUNCTION_TYPE));

    // Check the shared function info. Make sure it hasn't changed.
    __ li(a3, Operand(Handle<SharedFunctionInfo>(function->shared())));
    __ lw(t0, FieldMemOperand(a1, JSFunction::kSharedFunctionInfoOffset));
    __ Branch(&miss, ne, t0, Operand(a3));
  } else {
    __ Branch(&miss, ne, a1, Operand(Handle<JSFunction>(function)));
  }

  // Patch the receiver on the stack with the global proxy if
  // necessary.
  if (object->IsGlobalObject()) {
    __ lw(a3, FieldMemOperand(a0, GlobalObject::kGlobalReceiverOffset));
    __ sw(a3, MemOperand(sp, argc * kPointerSize));
  }

  __ nop(); __ nop(); __ nop(); __ nop(); __ nop(); __ nop(); __ nop();

  // Setup the context (function already in r1).
  __ lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));

  // Jump to the cached code (tail call).
  __ IncrementCounter(&Counters::call_global_inline, 1, a1, a3);
  ASSERT(function->is_compiled());
  Handle<Code> code(function->code());
  ParameterCount expected(function->shared()->formal_parameter_count());
  __ InvokeCode(code, expected, arguments(),
                RelocInfo::CODE_TARGET, JUMP_FUNCTION);

  // Handle call cache miss.
  __ bind(&miss);
  // __ break_(__LINE__);
  __ IncrementCounter(&Counters::call_global_inline_miss, 1, a1, a3);
  Handle<Code> ic = ComputeCallMiss(arguments().immediate());
  __ Jump(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(NORMAL, name);
}


Object* StoreStubCompiler::CompileStoreField(JSObject* object,
                                             int index,
                                             Map* transition,
                                             String* name) {
  // a0    : value
  // a1    : receiver
  // a2    : name
  // ra    : return address
  // [sp]  : receiver
  Label miss;

  // name register might be clobbered.
  GenerateStoreField(masm(),
                     object,
                     index,
                     transition,
                     a1, a2, a3,
                     &miss);
  __ bind(&miss);
  __ li(a2, Operand(Handle<String>(name)));  // Restore name.
  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Miss));
  // __ break_(0x452);
  __ JumpToBuiltin(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(transition == NULL ? FIELD : MAP_TRANSITION, name);
}


Object* StoreStubCompiler::CompileStoreCallback(JSObject* object,
                                                AccessorInfo* callback,
                                                String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* StoreStubCompiler::CompileStoreInterceptor(JSObject* receiver,
                                                   String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* StoreStubCompiler::CompileStoreGlobal(GlobalObject* object,
                                              JSGlobalPropertyCell* cell,
                                              String* name) {
  // a0    : value
  // a1    : receiver
  // a2    : name
  // ra    : return address
  Label miss;

  // Check that the map of the global has not changed.
  __ lw(a3, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ Branch(&miss, ne, a3, Operand(Handle<Map>(object->map())));

  // Store the value in the cell.
  __ li(a2, Operand(Handle<JSGlobalPropertyCell>(cell)));
  __ sw(a0, FieldMemOperand(a2, JSGlobalPropertyCell::kValueOffset));

  __ IncrementCounter(&Counters::named_store_global_inline, 1, a1, a3);
  __ Ret();

  // Handle store cache miss.
  __ bind(&miss);
  __ IncrementCounter(&Counters::named_store_global_inline_miss, 1, a1, a3);
  Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Miss));
  __ JumpToBuiltin(ic, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode(NORMAL, name);
}


Object* LoadStubCompiler::CompileLoadField(JSObject* object,
                                           JSObject* holder,
                                           int index,
                                           String* name) {
  // ----------- S t a t e -------------
  //  -- a2    : name
  //  -- ra    : return address
  //  -- [sp]  : receiver
  // -----------------------------------
  Label miss;

  __ lw(v0, MemOperand(sp, 0));

  GenerateLoadField(object, holder, v0, a3, a1, index, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(FIELD, name);
}


Object* LoadStubCompiler::CompileLoadCallback(String* name,
                                              JSObject* object,
                                              JSObject* holder,
                                              AccessorInfo* callback) {
  // a2    : name
  // ra    : return address
  // [sp]  : receiver
  Label miss;

  __ lw(a0, MemOperand(sp, 0));
  Failure* failure = Failure::InternalError();
  bool success = GenerateLoadCallback(object, holder, a0, a2, a3, a1,
                                      callback, name, &miss, &failure);
  if (!success) return failure;

  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(CALLBACKS, name);
}


Object* LoadStubCompiler::CompileLoadConstant(JSObject* object,
                                              JSObject* holder,
                                              Object* value,
                                              String* name) {
  // a2    : name
  // ra    : return address
  // [sp] : receiver
  Label miss;

  __ lw(a0, MemOperand(sp, 0));

  GenerateLoadConstant(object, holder, a0, a3, a1, value, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(CONSTANT_FUNCTION, name);
}


Object* LoadStubCompiler::CompileLoadInterceptor(JSObject* object,
                                                 JSObject* holder,
                                                 String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* LoadStubCompiler::CompileLoadGlobal(JSObject* object,
                                            GlobalObject* holder,
                                            JSGlobalPropertyCell* cell,
                                            String* name,
                                            bool is_dont_delete) {
  // a2    : name
  // ra    : return address
  // [sp]  : receiver
  Label miss;

  // Get the receiver from the stack.
  __ lw(a1, MemOperand(sp));

  // If the object is the holder then we know that it's a global
  // object which can only happen for contextual calls. In this case,
  // the receiver cannot be a smi.
  if (object != holder) {
    __ And(t0, a1, Operand(kSmiTagMask));
    __ Branch(&miss, eq, t0, Operand(zero_reg));
  }

  // Check that the map of the global has not changed.
  CheckPrototypes(object, a1, holder, a3, a0, name, &miss);

  // Get the value from the cell.
  __ li(a3, Operand(Handle<JSGlobalPropertyCell>(cell)));
  __ lw(v0, FieldMemOperand(a3, JSGlobalPropertyCell::kValueOffset));

  // Check for deleted property if property can actually be deleted.
  if (!is_dont_delete) {
    __ LoadRoot(t0, Heap::kTheHoleValueRootIndex);
    __ Branch(&miss, eq, v0, Operand(t0));
  }

  __ IncrementCounter(&Counters::named_load_global_inline, 1, a1, a3);
  __ Ret();

  __ bind(&miss);
  __ IncrementCounter(&Counters::named_load_global_inline_miss, 1, a1, a3);
  GenerateLoadMiss(masm(), Code::LOAD_IC);

  // Return the generated code.
  return GetCode(NORMAL, name);
}


Object* KeyedLoadStubCompiler::CompileLoadField(String* name,
                                                JSObject* receiver,
                                                JSObject* holder,
                                                int index) {
  // ----------- S t a t e -------------
  //  -- ra    : return address
  //  -- sp[0] : key
  //  -- sp[4] : receiver
  // -----------------------------------
  Label miss;

  __ lw(a2, MemOperand(sp, 0));
  __ lw(a0, MemOperand(sp, kPointerSize));

  __ Branch(&miss, ne, a2, Operand(Handle<String>(name)));

  GenerateLoadField(receiver, holder, a0, a3, a1, index, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  return GetCode(FIELD, name);
}


Object* KeyedLoadStubCompiler::CompileLoadCallback(String* name,
                                                   JSObject* receiver,
                                                   JSObject* holder,
                                                   AccessorInfo* callback) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadConstant(String* name,
                                                   JSObject* receiver,
                                                   JSObject* holder,
                                                   Object* value) {
  // ra    : return address
  // sp[0] : key
  // sp[4] : receiver
  Label miss;

  // Check the key is the cached one
  __ lw(a2, MemOperand(sp, 0));
  __ lw(a0, MemOperand(sp, kPointerSize));

  __ Branch(&miss, ne, a2, Operand(Handle<String>(name)));

  __ break_(__LINE__);
  GenerateLoadConstant(receiver, holder, a0, a3, a1, value, name, &miss);
  __ bind(&miss);
  GenerateLoadMiss(masm(), Code::KEYED_LOAD_IC);

  // Return the generated code.
  return GetCode(CONSTANT_FUNCTION, name);
}


Object* KeyedLoadStubCompiler::CompileLoadInterceptor(JSObject* receiver,
                                                      JSObject* holder,
                                                      String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadArrayLength(String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadStringLength(String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


// TODO(1224671): implement the fast case.
Object* KeyedLoadStubCompiler::CompileLoadFunctionPrototype(String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedStoreStubCompiler::CompileStoreField(JSObject* object,
                                                  int index,
                                                  Map* transition,
                                                  String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(__LINE__);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* ConstructStubCompiler::CompileConstructStub(
    SharedFunctionInfo* shared) {
  // a0    : argc
  // a1    : constructor
  // ra    : return address
  // [sp]  : last argument
  Label generic_stub_call;

  // Use t7 for holding undefined which is used in several places below.
  __ LoadRoot(t7, Heap::kUndefinedValueRootIndex);

#ifdef ENABLE_DEBUGGER_SUPPORT
  // Check to see whether there are any break points in the function code. If
  // there are jump to the generic constructor stub which calls the actual
  // code for the function thereby hitting the break points.
  __ lw(t5, FieldMemOperand(a1, JSFunction::kSharedFunctionInfoOffset));
  __ lw(a2, FieldMemOperand(t5, SharedFunctionInfo::kDebugInfoOffset));
  __ Branch(&generic_stub_call, ne, a2, Operand(t7));
#endif

  // Load the initial map and verify that it is in fact a map.
  // a1: constructor function
  // t7: undefined
  __ lw(a2, FieldMemOperand(a1, JSFunction::kPrototypeOrInitialMapOffset));
  __ And(t0, a2, Operand(kSmiTagMask));
  __ Branch(&generic_stub_call, eq, t0, Operand(zero_reg));
  __ GetObjectType(a2, a3, t0);
  __ Branch(&generic_stub_call, ne, t0, Operand(MAP_TYPE));

#ifdef DEBUG
  // Cannot construct functions this way.
  // a0: argc
  // a1: constructor function
  // a2: initial map
  // t7: undefined
  __ lbu(a3, FieldMemOperand(a2, Map::kInstanceTypeOffset));
  __ Check(ne, "Function constructed by construct stub.",
      a3, Operand(JS_FUNCTION_TYPE));
#endif

  // Now allocate the JSObject in new space.
  // a0: argc
  // a1: constructor function
  // a2: initial map
  // t7: undefined
  __ lbu(a3, FieldMemOperand(a2, Map::kInstanceSizeOffset));
  __ AllocateInNewSpace(a3,
                        t4,
                        t5,
                        t6,
                        &generic_stub_call,
                        NO_ALLOCATION_FLAGS);

  // Allocated the JSObject, now initialize the fields. Map is set to initial
  // map and properties and elements are set to empty fixed array.
  // a0: argc
  // a1: constructor function
  // a2: initial map
  // a3: object size (in words)
  // t4: JSObject (not tagged)
  // r7: undefined
  __ LoadRoot(t6, Heap::kEmptyFixedArrayRootIndex);
  __ mov(t5, t4);
  __ sw(a2, MemOperand(t5, JSObject::kMapOffset));
  __ sw(t6, MemOperand(t5, JSObject::kPropertiesOffset));
  __ sw(t6, MemOperand(t5, JSObject::kElementsOffset));
  __ Addu(t5, t5, Operand(3 * kPointerSize));
  ASSERT_EQ(0 * kPointerSize, JSObject::kMapOffset);
  ASSERT_EQ(1 * kPointerSize, JSObject::kPropertiesOffset);
  ASSERT_EQ(2 * kPointerSize, JSObject::kElementsOffset);


  // Calculate the location of the first argument. The stack contains only the
  // argc arguments.
  __ sll(a1, a0, kPointerSizeLog2);
  __ Addu(a1, a1, sp);

  // We need to add the 4 args slots size because they need to be setup when we
  // call the real time function the first time this kind of object is
  // initialized (cf Builtins::Generate_JSConstructStubGeneric).
  // TOCHECK: This need is maybe just because I first implemented it with args
  // slots. Try to do it without: we should not need this as the real time
  // function called has the stack setup just before it is called.
//  __ Addu(a1, a1, StandardFrameConstants::kRArgsSlotsSize);

  // Fill all the in-object properties with undefined.
  // a0: argc
  // a1: first argument
  // a3: object size (in words)
  // t4: JSObject (not tagged)
  // t5: First in-object property of JSObject (not tagged)
  // t7: undefined
  // Fill the initialized properties with a constant value or a passed argument
  // depending on the this.x = ...; assignment in the function.
  for (int i = 0; i < shared->this_property_assignments_count(); i++) {
    if (shared->IsThisPropertyAssignmentArgument(i)) {
      Label not_passed, next;
      // Check if the argument assigned to the property is actually passed.
      int arg_number = shared->GetThisPropertyAssignmentArgument(i);
      __ Branch(&not_passed, less_equal, a0, Operand(arg_number));
      // Argument passed - find it on the stack.
      __ lw(a2, MemOperand(a1, (arg_number + 1) * -kPointerSize));
      __ sw(a2, MemOperand(t5));
      __ Addu(t5, t5, kPointerSize);
      __ jmp(&next);
      __ bind(&not_passed);
      // Set the property to undefined.
      __ sw(t7, MemOperand(t5));
      __ Addu(t5, t5, Operand(kPointerSize));
      __ bind(&next);
    } else {
      // Set the property to the constant value.
      Handle<Object> constant(shared->GetThisPropertyAssignmentConstant(i));
      __ li(a2, Operand(constant));
      __ sw(a2, MemOperand(t5));
      __ Addu(t5, t5, kPointerSize);
    }
  }

  // Fill the unused in-object property fields with undefined.
  for (int i = shared->this_property_assignments_count();
       i < shared->CalculateInObjectProperties();
       i++) {
      __ sw(t7, MemOperand(t5));
      __ Addu(t5, t5, kPointerSize);
  }

  // a0: argc
  // t4: JSObject (not tagged)
  // Move argc to a1 and the JSObject to return to v0 and tag it.
  __ mov(a1, a0);
  __ mov(v0, t4);
  __ Or(v0, v0, Operand(kHeapObjectTag));

  // v0: JSObject
  // a1: argc
  // Remove caller arguments and receiver from the stack and return.
  __ sll(t0, a1, kPointerSizeLog2);
  __ Addu(sp, sp, t0);
  __ Addu(sp, sp, Operand(kPointerSize));
  __ IncrementCounter(&Counters::constructed_objects, 1, a1, a2);
  __ IncrementCounter(&Counters::constructed_objects_stub, 1, a1, a2);
  __ Ret();

  // Jump to the generic stub in case the specialized code cannot handle the
  // construction.
  __ bind(&generic_stub_call);
  Code* code = Builtins::builtin(Builtins::JSConstructStubGeneric);
  Handle<Code> generic_construct_stub(code);
  // __ break_(__LINE__);
  __ JumpToBuiltin(generic_construct_stub, RelocInfo::CODE_TARGET);

  // Return the generated code.
  return GetCode();
}


#undef __

} }  // namespace v8::internal

