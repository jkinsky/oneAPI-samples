# Makefile for NMAKE

all: factor.exe solve.exe
	.\factor.exe
	.\solve.exe

DPCPP_OPTS=/I"$(MKLROOT)\include" /Qmkl /Qmkl-sycl-impl="blas,lapack" /DMKL_ILP64 /EHsc -fsycl-device-code-split=per_kernel OpenCL.lib

factor.exe: factor.cpp dgeblttrf.cpp auxi.cpp
	icx-cl -fsycl factor.cpp dgeblttrf.cpp auxi.cpp /Fefactor.exe $(DPCPP_OPTS)

solve.exe: solve.cpp dgeblttrf.cpp dgeblttrs.cpp auxi.cpp
	icx-cl -fsycl solve.cpp dgeblttrf.cpp dgeblttrs.cpp auxi.cpp /Fesolve.exe $(DPCPP_OPTS)

clean:
	del /q factor.exe factor.exp factor.lib solve.exe solve.exp solve.lib
