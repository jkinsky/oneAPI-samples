#!/bin/bash
source /opt/intel/oneapi/setvars.sh > /dev/null 2>&1
/bin/echo "##" $(whoami) is compiling SYCL_Essentials Module7 -- oneDPL Extension APIs - 5 of 12 lower_bound.cpp
icpx -fsycl lab/lower_bound.cpp -std=c++17 -w -o bin/lower_bound
bin/lower_bound
