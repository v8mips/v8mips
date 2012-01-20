# Copyright 2008 the V8 project authors. All rights reserved.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
#       copyright notice, this list of conditions and the following
#       disclaimer in the documentation and/or other materials provided
#       with the distribution.
#     * Neither the name of Google Inc. nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import xml.etree.ElementTree as xml
import sys
import time
import threading
import string
import os

def getProperTestName(name):
  proper = name
  proper = filter(lambda x: not "--testing_serialization_file" in x, proper)
  proper = filter(lambda x: not "--test" == x, proper)
  proper = map(lambda x: string.replace(x, os.getcwd() + "/", ""), proper)
  proper = string.join(proper[1:], "_")
  return proper

class UnitTestOutput:
  class testCase:
    def __init__(self, name):
      self.root = xml.Element("testcase")
      self.root.attrib["name"] = name
      self.starttime = time.time()

    def finish(self, failed, failMsg = None):
      elapsed = time.time() - self.starttime
      self.root.attrib["time"] = str(round(elapsed,2))
      if failed:
        failure = xml.Element("failure")
        failure.text = failMsg
        self.root.append(failure)

    def getElement(self):
      return self.root

  class UnitTestLocal(threading.local):
    def __init__(self):
      self.curtest = None

  def __init__(self, testName):
    self.root = xml.Element("testsuite")
    self.root.attrib["name"] = testName
    self.local = UnitTestOutput.UnitTestLocal()
    self.local.curtest = None

  def startNewTest(self, name):
    if self.local.curtest != None:
      self.finishCurrentTest(False)
    self.local.curtest = self.testCase(name)

  def finishCurrentTest(self, newName, failed, failMsg = None):
    if self.local.curtest != None:
      self.local.curtest.finish(failed, failMsg)
      if newName:
        name = getProperTestName(newName)
        self.local.curtest.root.attrib["name"] = name
      self.root.append(self.local.curtest.getElement())
    else:
      sys.stderr.write("Tried to finish test but no test is active!")
    self.local.curtest = None

  def finishAndWrite(self, file):
    if self.local.curtest != None:
      self.finishCurrentTest(False)
    xml.ElementTree(self.root).write(file, "UTF-8")

