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
using ::v8::Value;
using ::v8::internal::StrLength;

namespace i = ::v8::internal;

TEST(MIPSFunctionCalls) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;

  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source =
    "function foo(arg1, arg2, arg3, arg4, arg5) {"
    "  return foo2(arg1, foo2(arg3, arg4));"
    "}"
    ""
    "function foo2(arg1, arg2) {"
    "  return arg2;"
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
    "  this.retArea = getSquareArea;"
    "  this.retPerim = function () {return 4 * this.c;}"
    "}"
    ""
    "function getSquareArea() {"
    "  return this.c * this.c;"
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
    "res = res + myGeom.inObj + myCircle.r"
    "+ myCircle2.r + myNewObj.newProperty;"
    "if (mySquare.retArea() == 100)"
    "  if (mySquare.retPerim() == 40)"
    "    res = res + mySquare.c;"
    "if (myLastObj instanceof LastGeomObject)"
    "  res = res + myLastObj.lastProperty;";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0xaaaaaa, script->Run()->Int32Value());
}



// Binary op tests start with well-behaved Smi values, then step thru
// corner cases, such as overflow from Smi value, to one Smi, one
// non-Smi, then to float cases.


TEST(MIPSBinaryAdd) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js = "function f() { var a=1023; var b=22249; return a + b; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(23272,  script->Run()->Int32Value());

  // Use of Literal
  js = "function f() { var a=1023; return a + 22249; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(23272,  script->Run()->Int32Value());

  // Use of Literal, with near-max Smi value (2^30 = 1073741824)
  js = "function f() { var a=1073741822; return a + 1; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(1073741823,  script->Run()->Int32Value());

  // Use of Literal, near-max Smi value, overflow to Number
  js = "function f() { var a=1073741822; return a + 2; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(1073741824.0,  script->Run()->NumberValue());

  // Use of negative Literal, with near-min Smi value (2^30 = 1073741824)
  js = "function f() { var a=-1073741823; return a + -1; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(-1073741824,  script->Run()->Int32Value());

  // Use of negative Literal, near-min Smi, with underflow
  js = "function f() { var a=-1073741823; return a + -2; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(-1073741825.0,  script->Run()->NumberValue());

  // test add with result which overflows an Smi value.
  js = "function f() {"
       "  var a=0x20000000; var b=0x30000000; return a + b;"
       "};"
       "f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  // result 0x50000000 == 1,342,177,280, convert to Number (double).
  CHECK_EQ(1342177280.0,  script->Run()->NumberValue());

  js = "function f() { var a=1.0; var b=2.2; return a + b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(3.2,  script->Run()->NumberValue());
}


TEST(MIPSBinarySub) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js = "function f() { var a=1023; var b=734; return a - b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(289, script->Run()->Int32Value());

  // Check Smi sub literal.
  js = "function f() { var a=1023; return a - 23; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(1000, script->Run()->Int32Value());

  // Check literal sub Smi.
  js = "function f() { var a=1023; return 2048 - a; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(1025, script->Run()->Int32Value());

  // Check large negative numbers, all Smi.
  js = "function f() {"
       "  var a=-500000123; var b=400000000; return a - b;"
       "};"
       "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-900000123, script->Run()->Int32Value());

  // Check result overflows Smi, convert to HeapNumber.
  js = "function f() {"
       "  var a=-800000123; var b=600000000; return a - b;"
       "};"
       "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-1400000123.0, script->Run()->NumberValue());

  // Use of Literal, with near-min Smi value (2^30 = 1073741824)
  js = "function f() { var a=-1073741823; return a - 1; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(-1073741824,  script->Run()->Int32Value());

  // Use of Literal, near-min Smi, underflow, convert to Number
  js = "function f() { var a=-1073741823; return a - 2; }; f();";
  source = ::v8::String::New(js);
  script  = ::v8::Script::Compile(source);
  CHECK_EQ(-1073741825.0,  script->Run()->NumberValue());


  // Check doubles.
  js = "function f() { var a=14778.223; var b=278.220; return a - b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(14500.003, script->Run()->NumberValue());
}


TEST(MIPSBinaryMul) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  // Check basic Smi multiply.
  js = "function f() { var a=1023; var b=9936; return a * b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(10164528, script->Run()->Int32Value());

  // Check basic Smi multiply, with negative result.
  js = "function f() { var a=1023; var b=-2244; return a * b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-2295612, script->Run()->Int32Value());

  // Check Smi multiply by literal.
  js = "function f() { var a=7777; return a * 7; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(54439, script->Run()->Int32Value());

  // Check positive Smi multiply by 0.
  js = "function f() { var a=112233; var b=0; return a * b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0, script->Run()->Int32Value());

  // Check negative Smi multiply by 0.
  // Per float rules, result is -0, which requires slow-path handling,
  // and therefore converts our Smi result to a HeapNumber.
  js = "function f() { var a=-112233; var b=0; return a * b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0.0, script->Run()->NumberValue());

  // Check result overflows Smi, convert to HeapNumber.
  js = "function f() { var a=325000000; var b=-4; return a * b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-1300000000.0, script->Run()->NumberValue());

  // Check basic float multiply.
  js = "function f() { var a=1745.34; var b=37.1; return a * b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(64752.114, script->Run()->NumberValue());
}


TEST(MIPSBinaryDiv) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  // Check basic Smi divide.
  js = "function f() { var a=10; var b=5; return a / b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(2, script->Run()->Int32Value());

  js = "function f() { var a=499998015; var b=4455; return a / b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(112233, script->Run()->Int32Value());

  // Check Smi divide, with fractional (Number) result.
  js = "function f() { var a=5; var b=10; return a / b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0.5, script->Run()->NumberValue());

  // Check Smi divide by literal.
  js = "function f() { var a=10; return a / 5; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(2, script->Run()->Int32Value());

  // Check literal divide by Smi.
  js = "function f() { var a=10; return 500 / a; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(50, script->Run()->Int32Value());

  // Check negative 0 result causes conversion to HeapNumber.
  js = "function f() { var a=0; var b=-74; return a / b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0.0, script->Run()->NumberValue());

  // Check division of most-negative Smi by -1, to make illegal Smi.
  // results in conversion to legal HeapNumber.
  js = "function f() { var a=-1073741820; var b=-1; return a / b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(1073741820.0, script->Run()->NumberValue());

  // Check normal operation with doubles.
  js = "function f() { var a=173.5; var b=2.5; return a / b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(69.4, script->Run()->NumberValue());
}


TEST(MIPSBinaryMod) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  // Check basic Smi modulo.
  js = "function f() { var a=40015; var b=100; return a % b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(15, script->Run()->Int32Value());

  js = "function f() { var a=-44; var b=10; return a % b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-4, script->Run()->Int32Value());

  // Check Smi mod power-of-two literal.
  js = "function f() { var a=0x8833; return a % 256; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x33, script->Run()->Int32Value());

  // Check negative 0 result causes conversion to HeapNumber.
  js = "function f() { var a=-44; var b=11; return a % b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0.0, script->Run()->NumberValue());

  js = "function f() { var a=10.5; var b=3.0; return a % b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(1.5, script->Run()->NumberValue());
}


TEST(MIPSBinaryOr) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js = "function f() { var a=0xf0101; var b=0x948282; return a | b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x9f8383,  script->Run()->Int32Value());

  // Check Smi or with literal.
  js = "function f() { var a=0x1000; return  a | 0x34; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1034,  script->Run()->Int32Value());

  // Check that non-Smi int32 values work, converted to Number.
  js =
  "function f() { var a=0x55555555; var b=0x66666666; return a | b; };"
  "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  // 0x77777777 = 2004318071.
  CHECK_EQ(2004318071.0,  script->Run()->NumberValue());

  // Check that negative non-Smi int32 values work, returned as Smi.
  // -0x55555555 is too big for Smi, is Number with int32 value 0xaaaaaaab.
  // -0x22222222 has int value 0xddddddde, which is Smi 0xbbbbbbbc.
  // Or'ing these together gives 0xffffffff (-1), which is returned as Smi.
  js =
  "function f() { var a=-0x55555555; var b=-0x22222222; return a | b; };"
  "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-1,  script->Run()->Int32Value());
}


TEST(MIPSBinaryAnd) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js =
  "function f() { var a=0x0f0f0f0f; var b=0x11223344; return a & b; };"
  "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x01020304,  script->Run()->Int32Value());

  // Check Smi and with literal.
  js = "function f() { var a=0xffff; return a & 0x331248; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1248,  script->Run()->Int32Value());


  // Check that non-Smi values work OK, returned as Number.
  js =
  "function f() { var a=0x7f0f0f0f; var b=0x61223344; return a & b; };"
  "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  // 0x61020304 = 1627521796
  CHECK_EQ(1627521796.0, script->Run()->NumberValue());
}


TEST(MIPSBinaryXor) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js =
  "function f() { var a=0x0f0f0f0f; var b=0x11223344; return a ^ b; };"
  "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1e2d3c4b, script->Run()->Int32Value());

  // Check Smi xor with literal.
  js = "function f() { var a=0x0f0f0f0f; return 0x11223344 ^ a; };"
  "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1e2d3c4b, script->Run()->Int32Value());

  // Check two non-Smi int32's, with result returned as Smi.
  js =
  "function f() { var a=0x5f0f0f0f; var b=0x41223344; return a ^ b; };"
  "f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1e2d3c4b, script->Run()->Int32Value());
}


TEST(MIPSBinaryShl) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js = "function f() { var a=0x400; var b=0x4; return a << b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x4000, script->Run()->Int32Value());

  // Check Smi left-shift by literal.
  js = "function f() { var a=0x400; return a << 8; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x40000, script->Run()->Int32Value());

  // Check literal left-shift by Smi.
  js = "function f() { var a=12; return 0x0010 << a; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x10000, script->Run()->Int32Value());

  // Check left shift turning Smi to non-smi int32, returned as Number.
  js = "function f() { var a=0x30000000; var b=1; return a << b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  // 0x60000000 is 1610612736.
  CHECK_EQ(1610612736.0, script->Run()->NumberValue());
}


TEST(MIPSBinarySar) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js = "function f() { var a=-16; var b=4; return a >> b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-1, script->Run()->Int32Value());

  // Check Smi right-shifted by literal.
  js = "function f() { var a=-256; return a >> 4; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-16, script->Run()->Int32Value());

  // Check literal right-shifted by Smi.
  js = "function f() { var a=2; return 16 >> a; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(4, script->Run()->Int32Value());

  // Check that negative non-Smi int32 values work, returned as Smi.
  // -0x55555555 is too big for Smi, is Number with int32 value 0xaaaaaaab.
  // Right arithmetic shift of 8 gives 0xffaaaaaa, which is -5592406.
  js =
    "function f() { var a=-0x55555555; var b=8; return a >> b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(-5592406, script->Run()->Int32Value());
}


TEST(MIPSBinaryShr) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char* js;
  Local<String> source;
  Local<Script> script;

  js = "function f() { var a=-1; var b=0x4; return a >>> b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(0x0fffffff, script->Run()->Int32Value());

  // Check Smi right-shifted by literal.
  js = "function f() { var a=256; return a >>> 4; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(16, script->Run()->Int32Value());

  // Check literal right-shifted by Smi.
  js = "function f() { var a=2; return 64 >>> a; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  CHECK_EQ(16, script->Run()->Int32Value());

  // Check that almost-max negative non-Smi int32, shifted by 1, is
  // properly returned as number, since the positive value 0x40000000
  // cannot be represented as Smi. (we use 0x80000001, since due to
  // implementation detail, 0x80000000 is handled by 'Builtins').
  js =
    "function f() { var a=-2147483647; var b=1; return a >>> b; }; f();";
  source = ::v8::String::New(js);
  script = ::v8::Script::Compile(source);
  // -2147483647 is 0x80000001, we >>> 1, to get -1073741824
  CHECK_EQ(1073741824.0, script->Run()->NumberValue());
}

TEST(MIPSAddString) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h
  const char *js1, *js2, *js3;

  // Check adding two string variables.
  js1 = "function f() { var a='foo'; var b='bari'; return a + b; }; f();";
  Local<Value> result = CompileRun(js1);
  CHECK(result->IsString());
  String::AsciiValue ascii1(result);
  CHECK_EQ("foobari", *ascii1);

  // Check adding string variables and literals.
  js2 = "function f() {   "
        "  var a = 'foo'; "
        "  var b = 'doo'; "
        "  return a + 'bargooba' + b; "
        "}; "
        "f();";
  result = CompileRun(js2);
  CHECK(result->IsString());
  String::AsciiValue ascii2(result);
  CHECK_EQ("foobargoobadoo", *ascii2);

  // Check strings longer than the internal 13-char limit.
  js3 = "function f() {"
        "  var a = 'this is a longer string ';"
        "  var b = 'which will get beyond the 13 char flat-string limit';"
        "  return a + b;"
        "}; "
        " f(); ";
  result = CompileRun(js3);
  CHECK(result->IsString());
  String::AsciiValue ascii3(result);
  const char* expected = "this is a longer string which will "
                         "get beyond the 13 char flat-string limit";
  CHECK_EQ(expected, *ascii3);
}
