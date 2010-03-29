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

  const char* c_source = ""
    "function foo(arg1, arg2, arg3, arg4, arg5) {"
    "  return arg4;"
    "};"
    "foo(0x10, 0x20, 0x40, 0x80, 0x100);";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x80,  script->Run()->Int32Value());
}


TEST(MIPSComparisons) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;

  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = ""
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
  CHECK_EQ(0,  script->Run()->Int32Value());
}


TEST(MIPSIfThenElse) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;

  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = ""
    "if (1 < 9) {"
    "  if (555 <= 555)"
    "    if (999 > 998)"
    "      if (0 >= 0)"
    "        0x1111;"
    "} else {"
    "  0x4444;"
    "}";
  Local<String> source = ::v8::String::New(c_source);
  Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1111,  script->Run()->Int32Value());
}


// The binary-op tests are currently simple tests, with well-behaved Smi values.
// Corner cases, doubles, and overflows are not yet tested (because we know 
// they don't work).

TEST(MIPS_binary_add) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=1023; var b=22249; return a + b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(23272,  script->Run()->Int32Value());
}


TEST(MIPS_binary_sub) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=1023; var b=734; return a - b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(289,  script->Run()->Int32Value());
}


TEST(MIPS_binary_mul) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=1023; var b=9936; return a * b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(10164528,  script->Run()->Int32Value());
}


TEST(MIPS_binary_div) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=499998015; var b=4455; return a / b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(112233,  script->Run()->Int32Value());
}


TEST(MIPS_binary_mod) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=40015; var b=100; return a % b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(15,  script->Run()->Int32Value());
}


TEST(MIPS_binary_or) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=0xf0101; var b=0x948282; return a | b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x9f8383,  script->Run()->Int32Value());
}


TEST(MIPS_binary_and) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=0x0f0f0f0f; var b=0x11223344; return a & b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x01020304,  script->Run()->Int32Value());
}


TEST(MIPS_binary_xor) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=0x0f0f0f0f; var b=0x11223344; return a ^ b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x1e2d3c4b,  script->Run()->Int32Value());
}


TEST(MIPS_binary_shl) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=0x400; var b=0x4; return a << b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(0x4000,  script->Run()->Int32Value());
}


TEST(MIPS_binary_sar) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=-16; var b=4; return a >> b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(-1,  script->Run()->Int32Value());
}


TEST(MIPS_binary_shr) {
  // Disable compilation of natives.
  i::FLAG_disable_native_files = true;
  i::FLAG_full_compiler = false;
  v8::HandleScope scope;
  LocalContext env;  // from cctest.h

  const char* c_source = 
        "function foo() { var a=-1; var b=0x4; return a >>> b; }; foo();";
  ::v8::Local<String> source = ::v8::String::New(c_source);
  ::v8::Local<Script> script = ::v8::Script::Compile(source);
  CHECK_EQ(268435455,  script->Run()->Int32Value());
}
