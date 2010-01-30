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
#include "fast-codegen.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm_)

// Generate code for a JS function.  On entry to the function the receiver
// and arguments have been pushed on the stack left to right.  The actual
// argument count matches the formal parameter count expected by the
// function.
//
// The live registers are:
//   - a1: the JS function object being called (ie, ourselves)
//   - cp: our context
//   - fp: our caller's frame pointer
//   - sp: stack pointer
//   - ra: return address
//
// The function builds a JS frame.  Please see JavaScriptFrameConstants in
// frames-arm.h for its layout.
void FastCodeGenerator::Generate(FunctionLiteral* fun) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::EmitReturnSequence(int position) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::Move(Expression::Context context, Register source) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::Move(Expression::Context context, Slot* source) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::Move(Expression::Context context, Literal* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::DropAndMove(Expression::Context context,
                                    Register source) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::TestAndBranch(Register source,
                                      Label* true_label,
                                      Label* false_label) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitReturnStatement(ReturnStatement* stmt) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitFunctionLiteral(FunctionLiteral* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitVariableProxy(VariableProxy* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitRegExpLiteral(RegExpLiteral* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitObjectLiteral(ObjectLiteral* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitArrayLiteral(ArrayLiteral* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::EmitVariableAssignment(Assignment* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::EmitNamedPropertyAssignment(Assignment* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::EmitKeyedPropertyAssignment(Assignment* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitProperty(Property* expr) {
  UNIMPLEMENTED();
}

void FastCodeGenerator::EmitCallWithIC(Call* expr, RelocInfo::Mode reloc_info) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::EmitCallWithStub(Call* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitCall(Call* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitCallNew(CallNew* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitCallRuntime(CallRuntime* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitUnaryOperation(UnaryOperation* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitCountOperation(CountOperation* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitBinaryOperation(BinaryOperation* expr) {
  UNIMPLEMENTED();
}


void FastCodeGenerator::VisitCompareOperation(CompareOperation* expr) {
  UNIMPLEMENTED();
}


#undef __


} }  // namespace v8::internal

