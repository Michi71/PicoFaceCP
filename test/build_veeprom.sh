#!/bin/bash
set -e
cd "$(dirname "$0")"
c++ -std=c++17 -O1 -Wall -DVEEPROM_HOST_TEST -I../include veeprom_test.cpp ../src/veeprom.cpp -o veeprom_test
./veeprom_test
