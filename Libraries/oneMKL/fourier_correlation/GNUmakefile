# Makefile for GNU Make

default: run_all

all: run_all

run_all: fcorr_1d_buff fcorr_1d_usm fcorr_2d_usm
	./fcorr_1d_buff 4096
	./fcorr_1d_usm 4096
	./fcorr_2d_usm

DPCPP_OPTS = -DMKL_ILP64 -qmkl -qmkl-sycl-impl="blas,dft,rng,vm"

fcorr_1d_buff: fcorr_1d_buffers.cpp
	icpx $< -fsycl -o $@ $(DPCPP_OPTS)

fcorr_1d_usm: fcorr_1d_usm.cpp
	icpx $< -fsycl -o $@ $(DPCPP_OPTS)

fcorr_2d_usm: fcorr_2d_usm.cpp
	icpx $< -fsycl -o $@ $(DPCPP_OPTS)

clean:
	-rm -f fcorr_1d_buff fcorr_1d_usm fcorr_2d_usm

.PHONY: run_all clean all
