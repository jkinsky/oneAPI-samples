# Makefile for GNU Make

default: run

all: matrix_mul_mkl

run: matrix_mul_mkl
	./matrix_mul_mkl single
	./matrix_mul_mkl double

INCLUDE_COMMON=../../../common
MKL_COPTS = -DMKL_ILP64  -qmkl -qmkl-sycl-impl=blas

DPCPP_OPTS = -O2 $(MKL_COPTS)

matrix_mul_mkl: matrix_mul_mkl.cpp
	icpx -fsycl -I$(INCLUDE_COMMON) $< -o $@ $(DPCPP_OPTS)

clean:
	-rm -f matrix_mul_mkl

.PHONY: clean run all
