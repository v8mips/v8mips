import imp
import optparse
import os
from os.path import join, dirname, abspath, basename, isdir, exists
import platform
import re
import signal
import subprocess
import sys
import tempfile
import time
import threading
import utils
import shutil
import resource


CORE_PATTERN = "core.%p"
CORE_PATTERN_FILE = "/proc/sys/kernel/core_pattern"
CORE_BASE_DIR = "cores.log"
LOCK = None
SUITEDIR = None
ENABLED = False
COUNTER = 0

SAVED_BINARIES = ["obj/test/debug/cctest", "obj/test/release/cctest", "shell", "shell_g", "d8", "d8_g"]

def init(spec_name=str(time.time())):
  global LOCK
  LOCK = threading.RLock()
  global SUITEDIR
  SUITEDIR = CORE_BASE_DIR + "/" + spec_name
  cur_os = utils.GuessOS()
  global ENABLED
  ENABLED = False
  global COUNTER
  COUNTER = 0
  if cur_os == "linux":
    if exists(CORE_PATTERN_FILE):
      f = open(CORE_PATTERN_FILE, "r")
      text = f.read().strip()
      if text == CORE_PATTERN:
        ENABLED = True
      else:
        print "Bad core pattern! Found: " + text + " expected: " + CORE_PATTERN
      rlimit = resource.getrlimit(resource.RLIMIT_CORE)[0]
      if rlimit != -1 and rlimit != resource.RLIM_INFINITY:
        print "Bad rlimit for core size!"
  else:
    print "Sorry, core dump handling only works on linux."

  if ENABLED:
    if not exists(CORE_BASE_DIR):
      os.mkdir(CORE_BASE_DIR)
    if exists(SUITEDIR):
      print SUITEDIR + " already exists! Not saving core dumps."
      ENABLED = False
    else:
      os.mkdir(SUITEDIR)

  if ENABLED:
    for file in SAVED_BINARIES:
      if exists(file):
        dst = SUITEDIR + "/" + basename(file)
        if "debug" in file:
          dst += "_g"
        shutil.copyfile(file, dst)

def HandleCoreDump(pid, name = None):
  "Can be called anytime, even if the test passed."
  if not ENABLED:
    return

  global LOCK
  with LOCK:
    corefile = "core." + str(pid)
    if exists(corefile):
      global COUNTER
      realname = "core." + str(COUNTER)
      if name:
        realname = name
      os.rename(corefile, SUITEDIR + "/" + name)
      COUNTER += 1
