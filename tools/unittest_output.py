import xml.etree.ElementTree as xml
import sys
import time
import threading

DEBUG = False

class UnitTestOutput:
    "A class that helps output JUnit/CppUnit-style xml"
    "Usage example:"
    "tu = UnitTestOutput('base')"
    "tu.startNewTest('testpass')"
    "tu.finishCurrentTest(False)"
    "tu.startNewTest('testfail')"
    "tu.finishCurrentTest(True, 'reason for failing'')"
    "tu.finishAndWrite(sys.stdout)"

    class testCase:
        def __init__(self, name):
            self.root = xml.Element("testcase")
            self.root.attrib["name"]=name
            self.starttime=time.time()
            if DEBUG:
              print name + " started"

        def finish(self, failed, failMsg = None):
            if DEBUG:
              print "finished"
            elapsed=time.time()-self.starttime
            self.root.attrib["time"]=str(round(elapsed,2))
            if failed:
                if DEBUG:
                  print failMsg
                failure = xml.Element("failure")
                failure.text=failMsg
                self.root.append(failure)

        def getElement(self):
            return self.root

    class UnitTestLocal(threading.local):
        def __init__(self):
            self.curtest = None

    def __init__(self, testName):
        if DEBUG:
          print testName
        self.root = xml.Element("testsuite")
        self.root.attrib["name"]=testName
        self.local = UnitTestOutput.UnitTestLocal()
        self.local.curtest = None

    def startNewTest(self, name):
        if self.local.curtest != None:
            self.finishCurrentTest(False)
        self.local.curtest = self.testCase(name)

    def finishCurrentTest(self, failed, failMsg = None):
        if self.local.curtest != None:
            self.local.curtest.finish(failed, failMsg)
            self.root.append(self.local.curtest.getElement())
        else:
            sys.stderr.write("Tried to finish test but no test is active!")
        self.local.curtest = None

    def finishAndWrite(self, file):
        if self.local.curtest != None:
            self.finishCurrentTest(False)
        xml.ElementTree(self.root).write(file, "UTF-8")

