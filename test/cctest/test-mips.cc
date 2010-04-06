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
#include "execution.h"

#include "cctest.h"

using ::v8::Local;
using ::v8::String;
using ::v8::Script;

namespace i = ::v8::internal;

TEST(MIPSFunctionCalls) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;

  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo(arg1, arg2, arg3, arg4, arg5) {"
    "	return foo2(arg1, foo2(arg3, arg4));"
    "}"
    ""
    "function foo2(arg1, arg2) {"
    "	return arg2;"
    "}"
    // We call the function twice because it needs more code.
    // TODO(MIPS): Detail what more is needed.
    "foo(1, 2, 3, 4, 5);"
    "foo(1, 2, 3, 4, 5);";

  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(4, script->Run()->Int32Value());
}


TEST(MIPSComparisons) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;

  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  // The "instanceof" statement is tested with objects in MIPSObjects.
  const char* c_source =
    "function foo() {"
    ""
    "  var nothing;"
    "  var n = 1234;"
    "  var s = '1234';"
    "  var bt = true;"
    "  var bf = false;"
    ""
    "  if (nothing == null)"
    "  if (typeof n == 'number')"
    "  if (typeof s == 'string')"
    "  if (typeof bt == 'boolean')"
    "  if (typeof bf == 'boolean')"
    "    return 0;"
    ""
    "  return 1;"
    "}"
    ""
    "foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0, script->Run()->Int32Value());
}


TEST(MIPSGlobalVariables) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;

  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "var nothing;"
    "var n = 1234;"
    "var s = '1234';"
    "var bt = true;"
    "var bf = false;"
    ""
    "var a = 0x0;"
    "var b = 0x123;"
    ""
    "if (nothing == null)"
    "if (typeof n == 'number')"
    "if (typeof s == 'string')"
    "if (typeof bt == 'boolean')"
    "if (typeof bf == 'boolean') {"
    "  a = b;"
    "  a;"
    "}";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x123, script->Run()->Int32Value());
}


TEST(MIPSControlFlow) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;

  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "var res = 0;"
    "var count = 100;"
    ""
    "if (1 < 9)"
    "  if (555 <= 555)"
    "    if (999 > 998)"
    "      if (0 >= 0)"
    "        res = 0xa;"
    ""
    "while (count > 90) {"
    "  count = count - 1;"
    "  res = res + 0x10;"
    "}"
    ""
    "do {"
    "  count = count - 1;"
    "  res = res + 0x100;"
    "} while (count > 80);"
    ""
    "while (count > 60) {"
    "  count = count - 1;"
    "  if (count >= 70)"
    "    continue;"
    "  res = res + 0x1000;"
    "}"
    ""
    "while (count > 40) {"
    "  count = count - 1;"
    "  res = res + 0x10000;"
    "  if (count <= 50)"
    "    break;"
    "}"
    ""
    "while (count > 30) {"
    "  switch (count) {"
    "    case 39:"
    "      count = count - 1;"
    "      res = res + 0x100000;"
    "      break;"
    ""
    "    case 33:"
    "      count = count - 1;"
    "      res = res + 0x900000;"
    ""
    "    default:"
    "      count = count - 1;"
    "  }"
    "}"
    ""
    "for (var i = 0; i < 10; i = i + 1) {"
    "  count = count - 1;"
    "  res = res + 0x1000000;"
    "}"
    ""
    "res;";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0xaaaaaaa, script->Run()->Int32Value());
}


TEST(MIPSUnaryOperations) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "var res = 0x1233;"
    "var b = false;"
    "var qwerty;"
    ""
    "if (!qwerty)"
    "  res = res + 0x1;"
    ""
    "typeof res;"
    ""
    "~res;";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0xffffedcb, script->Run()->Int32Value());
}


TEST(MIPSCountOperation) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "var c = 0;"
    "for ( var i = 0; i < 50; i++)"
    "  ++c;"
    "c;";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(50, script->Run()->Int32Value());
}


TEST(MIPSArrays) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "myArray = [];"
    "myArray[1] = 0x10;"
    "myArray[2] = 0x20;"
    "myArray[3] = 0x30;"
    "myArray[2];";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x20, script->Run()->Int32Value());
}


TEST(MIPSObjects) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    // Global variable to store the result.
    "var res = 0;"
    ""
    // Constructors.
    "function GeomObject() {}"
    ""
    "function Square(c_) {"
    "  this.c = c_;"
    "}"
    ""
    "function Circle() {"
    "  this.x = 0;"
    "  this.y = 0;"
    "  this.r = 0;"
    "}"
    ""
    "NewGeomObject.prototype = new GeomObject;"
    "NewGeomObject.prototype.constructor = NewGeomObject;"
    "function NewGeomObject() {"
    "  this.newProperty = 0xa0000;"
    "}"
    ""
    "LastGeomObject.prototype = new NewGeomObject;"
    "LastGeomObject.prototype.constructor = LastGeomObject;"
    "function LastGeomObject() {"
    "  this.lastProperty = 0xa00000;"
    "}"
    ""
    // Instantiate objects.
    "myGeom = new GeomObject;"
    "mySquare = new Square(0xa);"
    "myCircle = new Circle;"
    "myCircle2 = new Circle;"
    "myNewObj = new NewGeomObject;"
    "myLastObj = new LastGeomObject;"
    ""
    // Change object prototype.
    "GeomObject.prototype.inObj = 0xa0;"
    ""
    // Change object properties.
    "myCircle.r = 0xa00;"
    "myCircle2.r = 0xa000;"
    ""
    // Compute a result involving all previous aspects.
    "res = mySquare.c + myGeom.inObj + myCircle.r + myCircle2.r"
    "+ myNewObj.newProperty;"
    "if (myLastObj instanceof LastGeomObject)"
    "  res = res + myLastObj.lastProperty;";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0xaaaaaa, script->Run()->Int32Value());
}


// The binary-op tests are currently simple tests, with well-behaved Smi values.
// Corner cases, doubles, and overflows are not yet tested (because we know
// they don't work).

TEST(MIPSBinaryAdd) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=1023; var b=22249; return a + b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(23272, script->Run()->Int32Value());
}


TEST(MIPSBinarySub) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=1023; var b=734; return a - b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(289, script->Run()->Int32Value());
}


TEST(MIPSBinaryMul) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=1023; var b=9936; return a * b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(10164528, script->Run()->Int32Value());
}


TEST(MIPSBinaryDiv) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=499998015; var b=4455; return a / b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(112233, script->Run()->Int32Value());
}


TEST(MIPSBinaryMod) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=40015; var b=100; return a % b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(15, script->Run()->Int32Value());
}


TEST(MIPSBinaryOr) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=0xf0101; var b=0x948282; return a | b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x9f8383, script->Run()->Int32Value());
}


TEST(MIPSBinaryAnd) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=0x0f0f0f0f; var b=0x11223344; return a & b; };"
    "foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x01020304, script->Run()->Int32Value());
}


TEST(MIPSBinaryXor) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=0x0f0f0f0f; var b=0x11223344; return a ^ b; };"
    "foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1e2d3c4b, script->Run()->Int32Value());
}


TEST(MIPSBinaryShl) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=0x400; var b=0x4; return a << b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x4000, script->Run()->Int32Value());
}


TEST(MIPSBinarySar) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=-16; var b=4; return a >> b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(-1, script->Run()->Int32Value());
}


TEST(MIPSBinaryShr) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo() { var a=-1; var b=0x4; return a >>> b; }; foo();";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(268435455, script->Run()->Int32Value());
}
