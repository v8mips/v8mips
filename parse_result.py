#!/usr/bin/python

import csv
import sys
import os
from collections import *

def parseFiles(files):
  data = dict()
  file_num = 1
  for f in files:
    print("Processing #" + str(file_num) + ": " + f)
    with open(f) as fileobject:
      for line in fileobject:
        if line.find("LIS:") != -1:
          r = line.replace("LIS:", "").replace("\n", "").split("\t")
          key = r[0]
          name = r[1]
          size = r[2]
          name_with_params = r[3]
          if key not in data:
            data[key] = {"name":name, "size":size, "count":0, "params":{name_with_params:0}}
          data[key]["count"] += 1
          if name_with_params not in data[key]["params"]:
            data[key]["params"][name_with_params] = 0
          data[key]["params"][name_with_params] += 1
      file_num += 1
  return data

def writeDataToCsv(data, out_file_name):
  with open(out_file_name, "wb") as csvfile:
    print("Generating result...")
    csvout = csv.writer(csvfile, delimiter=',', quotechar='"', quoting=csv.QUOTE_MINIMAL)
    for k in sorted(data.iterkeys()):
      v = data[k]
      csvout.writerow([k, v["name"], v["size"], v["count"]])

def Main():
  input_file = sys.argv[1]
  if not os.path.isfile(input_file):
    # Process the whole directory, if the argument is a directory
    input_file = [os.path.join(input_file, f) for f in os.listdir(input_file)
                  if os.path.isfile(os.path.join(input_file,f))]
  else:
    input_file = [input_file]
  out_file_name = sys.argv[2]

  data = parseFiles(input_file)
  writeDataToCsv(data, out_file_name)

if __name__ == "__main__":
  sys.exit(Main())
