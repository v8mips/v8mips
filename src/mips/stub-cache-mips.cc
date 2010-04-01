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


void StubCache::GenerateProbe(MacroAssembler* masm,
                              Code::Flags flags,
                              Register receiver,
                              Register name,
                              Register scratch,
                              Register extra) {
  UNIMPLEMENTED_MIPS();
}


void StubCompiler::GenerateLoadGlobalFunctionPrototype(MacroAssembler* masm,
                                                       int index,
                                                       Register prototype) {
  UNIMPLEMENTED_MIPS();
}


// Load a fast property out of a holder object (src). In-object properties
// are loaded directly otherwise the property is loaded from the properties
// fixed array.
void StubCompiler::GenerateFastPropertyLoad(MacroAssembler* masm,
                                            Register dst, Register src,
                                            JSObject* holder, int index) {
  UNIMPLEMENTED_MIPS();
}


void StubCompiler::GenerateLoadArrayLength(MacroAssembler* masm,
                                           Register receiver,
                                           Register scratch,
                                           Label* miss_label) {
  UNIMPLEMENTED_MIPS();
}


void StubCompiler::GenerateLoadFunctionPrototype(MacroAssembler* masm,
                                                 Register receiver,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 Label* miss_label) {
  UNIMPLEMENTED_MIPS();
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
  UNIMPLEMENTED_MIPS();
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
      __ Branch(ne, miss, scratch, Operand(at));
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
  UNIMPLEMENTED_MIPS();
}


void StubCompiler::GenerateLoadConstant(JSObject* object,
                                        JSObject* holder,
                                        Register receiver,
                                        Register scratch1,
                                        Register scratch2,
                                        Object* value,
                                        String* name,
                                        Label* miss) {
  UNIMPLEMENTED_MIPS();
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
  __ break_(0x470);
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
  __ break_(0x505);
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
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* CallStubCompiler::CompileArrayPushCall(Object* object,
                                               JSObject* holder,
                                               JSFunction* function,
                                               String* name,
                                               CheckType check) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* CallStubCompiler::CompileArrayPopCall(Object* object,
                                              JSObject* holder,
                                              JSFunction* function,
                                              String* name,
                                              CheckType check) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* CallStubCompiler::CompileCallConstant(Object* object,
                                              JSObject* holder,
                                              JSFunction* function,
                                              String* name,
                                              CheckType check) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* CallStubCompiler::CompileCallInterceptor(JSObject* object,
                                                 JSObject* holder,
                                                 String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x782);
  return GetCode(INTERCEPTOR, name);
}


Object* CallStubCompiler::CompileCallGlobal(JSObject* object,
                                            GlobalObject* holder,
                                            JSGlobalPropertyCell* cell,
                                            JSFunction* function,
                                            String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* StoreStubCompiler::CompileStoreField(JSObject* object,
                                             int index,
                                             Map* transition,
                                             String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* StoreStubCompiler::CompileStoreCallback(JSObject* object,
                                                AccessorInfo* callback,
                                                String* name) {
  UNIMPLEMENTED_MIPS();
  __ break_(0x906);
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* StoreStubCompiler::CompileStoreInterceptor(JSObject* receiver,
                                                   String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* StoreStubCompiler::CompileStoreGlobal(GlobalObject* object,
                                              JSGlobalPropertyCell* cell,
                                              String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* LoadStubCompiler::CompileLoadField(JSObject* object,
                                           JSObject* holder,
                                           int index,
                                           String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* LoadStubCompiler::CompileLoadCallback(String* name,
                                              JSObject* object,
                                              JSObject* holder,
                                              AccessorInfo* callback) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* LoadStubCompiler::CompileLoadConstant(JSObject* object,
                                              JSObject* holder,
                                              Object* value,
                                              String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* LoadStubCompiler::CompileLoadInterceptor(JSObject* object,
                                                 JSObject* holder,
                                                 String* name) {
  UNIMPLEMENTED_MIPS();
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
    __ Branch(eq, &miss, t0, Operand(zero_reg));
  }

  // Check that the map of the global has not changed.
  CheckPrototypes(object, a1, holder, a3, a0, name, &miss);

  // Get the value from the cell.
  __ li(a3, Operand(Handle<JSGlobalPropertyCell>(cell)));
  __ lw(v0, FieldMemOperand(a3, JSGlobalPropertyCell::kValueOffset));

  // Check for deleted property if property can actually be deleted.
  if (!is_dont_delete) {
    __ LoadRoot(t0, Heap::kTheHoleValueRootIndex);
    __ Branch(eq, &miss, v0, Operand(t0));
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
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadCallback(String* name,
                                                   JSObject* receiver,
                                                   JSObject* holder,
                                                   AccessorInfo* callback) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadConstant(String* name,
                                                   JSObject* receiver,
                                                   JSObject* holder,
                                                   Object* value) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadInterceptor(JSObject* receiver,
                                                      JSObject* holder,
                                                      String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadArrayLength(String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedLoadStubCompiler::CompileLoadStringLength(String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


// TODO(1224671): implement the fast case.
Object* KeyedLoadStubCompiler::CompileLoadFunctionPrototype(String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* KeyedStoreStubCompiler::CompileStoreField(JSObject* object,
                                                  int index,
                                                  Map* transition,
                                                  String* name) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


Object* ConstructStubCompiler::CompileConstructStub(
    SharedFunctionInfo* shared) {
  UNIMPLEMENTED_MIPS();
  return reinterpret_cast<Object*>(NULL);   // UNIMPLEMENTED RETURN
}


#undef __

} }  // namespace v8::internal

