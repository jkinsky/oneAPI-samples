/* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sycl/sycl.hpp>
#include <dpct/dpct.hpp>
#include <helper_cuda.h>
#include <vector>
#include "jacobi.h"

// 8 Rows of square-matrix A processed by each CTA.
// This can be max 32 and only power of 2 (i.e., 2/4/8/16/32).
#define ROWS_PER_CTA 8

#if !defined(DPCT_COMPATIBILITY_TEMP) || DPCT_COMPATIBILITY_TEMP >= 600
#else
__device__ double atomicAdd(double *address, double val) {
  unsigned long long int *address_as_ull = (unsigned long long int *)address;
  unsigned long long int old = *address_as_ull, assumed;

  do {
    assumed = old;
    old = atomicCAS(address_as_ull, assumed,
                    __double_as_longlong(val + __longlong_as_double(assumed)));

    // Note: uses integer comparison to avoid hang in case of NaN (since NaN !=
    // NaN)
  } while (assumed != old);

  return __longlong_as_double(old);
}
#endif

static void JacobiMethod(const float *A, const double *b,
                                    const float conv_threshold, double *x,
                                    double *x_new, double *sum,
                                    const sycl::nd_item<3> &item_ct1,
                                    double *x_shared, double *b_shared) {
  // Handle to thread block group
  sycl::group<3> cta = item_ct1.get_group();
    // N_ROWS == n

  for (int i = item_ct1.get_local_id(2); i < N_ROWS;
       i += item_ct1.get_local_range(2)) {
    x_shared[i] = x[i];
  }

  if (item_ct1.get_local_id(2) < ROWS_PER_CTA) {
    int k = item_ct1.get_local_id(2);
    for (int i = k + (item_ct1.get_group(2) * ROWS_PER_CTA);
         (k < ROWS_PER_CTA) && (i < N_ROWS);
         k += ROWS_PER_CTA, i += ROWS_PER_CTA) {
      b_shared[i % (ROWS_PER_CTA + 1)] = b[i];
    }
  }

  /*
  DPCT1065:0: Consider replacing sycl::nd_item::barrier() with
  sycl::nd_item::barrier(sycl::access::fence_space::local_space) for better
  performance if there is no access to global memory.
  */
  item_ct1.barrier();

  sycl::sub_group tile32 = item_ct1.get_sub_group();

  for (int k = 0, i = item_ct1.get_group(2) * ROWS_PER_CTA;
       (k < ROWS_PER_CTA) && (i < N_ROWS); k++, i++) {
    double rowThreadSum = 0.0;
    for (int j = item_ct1.get_local_id(2); j < N_ROWS;
         j += item_ct1.get_local_range(2)) {
      rowThreadSum += (A[i * N_ROWS + j] * x_shared[j]);
    }

    for (int offset = item_ct1.get_sub_group().get_local_linear_range() / 2;
         offset > 0; offset /= 2) {
      rowThreadSum += sycl::shift_group_left(item_ct1.get_sub_group(),
                                             rowThreadSum, offset);
    }

    if (item_ct1.get_sub_group().get_local_linear_id() == 0) {
      dpct::atomic_fetch_add<sycl::access::address_space::generic_space>(
          &b_shared[i % (ROWS_PER_CTA + 1)], -rowThreadSum);
    }
  }

  /*
  DPCT1065:1: Consider replacing sycl::nd_item::barrier() with
  sycl::nd_item::barrier(sycl::access::fence_space::local_space) for better
  performance if there is no access to global memory.
  */
  item_ct1.barrier();

  if (item_ct1.get_local_id(2) < ROWS_PER_CTA) {
    /*
    DPCT1119:33: Migration of cooperative_groups::__v1::thread_block_tile<8> is
    not supported, please try to remigrate with option:
    --use-experimental-features=logical-group.
    */
    cg::thread_block_tile<ROWS_PER_CTA> tile8 =
        /*
        DPCT1007:2: Migration of tiled_partition is not supported.
        */
        cg::tiled_partition<ROWS_PER_CTA>(cta);
    double temp_sum = 0.0;

    int k = item_ct1.get_local_id(2);

    for (int i = k + (item_ct1.get_group(2) * ROWS_PER_CTA);
         (k < ROWS_PER_CTA) && (i < N_ROWS);
         k += ROWS_PER_CTA, i += ROWS_PER_CTA) {
      double dx = b_shared[i % (ROWS_PER_CTA + 1)];
      dx /= A[i * N_ROWS + i];

      x_new[i] = (x_shared[i] + dx);
      temp_sum += sycl::fabs(dx);
    }

    /*
    DPCT1007:3: Migration of size is not supported.
    */
    for (int offset = tile8.size() / 2; offset > 0; offset /= 2) {
      temp_sum += dpct::shift_sub_group_left(item_ct1.get_sub_group(), temp_sum,
                                             offset, 8);
    }

    /*
    DPCT1007:4: Migration of thread_rank is not supported.
    */
    if (tile8.thread_rank() == 0) {
      dpct::atomic_fetch_add<sycl::access::address_space::generic_space>(
          sum, temp_sum);
    }
  }
}

// Thread block size for finalError kernel should be multiple of 32
static void finalError(double *x, double *g_sum,
                       const sycl::nd_item<3> &item_ct1, uint8_t *dpct_local) {
  // Handle to thread block group
  sycl::group<3> cta = item_ct1.get_group();
  auto warpSum = (double *)dpct_local;
  double sum = 0.0;

  int globalThreadId = item_ct1.get_group(2) * item_ct1.get_local_range(2) +
                       item_ct1.get_local_id(2);

  for (int i = globalThreadId; i < N_ROWS;
       i += item_ct1.get_local_range(2) * item_ct1.get_group_range(2)) {
    double d = x[i] - 1.0;
    sum += sycl::fabs(d);
  }

  sycl::sub_group tile32 = item_ct1.get_sub_group();

  for (int offset = item_ct1.get_sub_group().get_local_linear_range() / 2;
       offset > 0; offset /= 2) {
    sum += sycl::shift_group_left(item_ct1.get_sub_group(), sum, offset);
  }

  if (item_ct1.get_sub_group().get_local_linear_id() == 0) {
    warpSum[item_ct1.get_local_id(2) /
            item_ct1.get_sub_group().get_local_range().get(0)] = sum;
  }

  /*
  DPCT1065:5: Consider replacing sycl::nd_item::barrier() with
  sycl::nd_item::barrier(sycl::access::fence_space::local_space) for better
  performance if there is no access to global memory.
  */
  item_ct1.barrier();

  double blockSum = 0.0;
  if (item_ct1.get_local_id(2) <
      (item_ct1.get_local_range(2) /
       item_ct1.get_sub_group().get_local_range().get(0))) {
    blockSum = warpSum[item_ct1.get_local_id(2)];
  }

  if (item_ct1.get_local_id(2) < 32) {
    for (int offset = item_ct1.get_sub_group().get_local_linear_range() / 2;
         offset > 0; offset /= 2) {
      blockSum +=
          sycl::shift_group_left(item_ct1.get_sub_group(), blockSum, offset);
    }
    if (item_ct1.get_sub_group().get_local_linear_id() == 0) {
      dpct::atomic_fetch_add<sycl::access::address_space::generic_space>(
          g_sum, blockSum);
    }
  }
}

double JacobiMethodGpuCudaGraphExecKernelSetParams(
    const float *A, const double *b, const float conv_threshold,
    const int max_iter, double *x, double *x_new, dpct::queue_ptr stream) {
  // CTA size
  dpct::dim3 nthreads(256, 1, 1);
  // grid size
  dpct::dim3 nblocks((N_ROWS / ROWS_PER_CTA) + 2, 1, 1);
  /*
  DPCT1119:34: Migration of cudaGraph_t is not supported, please try to
  remigrate with option: --use-experimental-features=graph.
  */
  cudaGraph_t graph;
  /*
  DPCT1119:35: Migration of cudaGraphExec_t is not supported, please try to
  remigrate with option: --use-experimental-features=graph.
  */
  cudaGraphExec_t graphExec = NULL;

  double sum = 0.0;
  double *d_sum = NULL;
  checkCudaErrors(DPCT_CHECK_ERROR(
      d_sum = sycl::malloc_device<double>(1, dpct::get_in_order_queue())));

  /*
  DPCT1119:36: Migration of cudaGraphNode_t is not supported, please try to
  remigrate with option: --use-experimental-features=graph.
  */
  std::vector<cudaGraphNode_t> nodeDependencies;
  /*
  DPCT1119:37: Migration of cudaGraphNode_t is not supported, please try to
  remigrate with option: --use-experimental-features=graph.
  */
  cudaGraphNode_t memcpyNode, jacobiKernelNode, memsetNode;
  dpct::memcpy_parameter memcpyParams = {};
  cudaMemsetParams memsetParams = {0};

  memsetParams.dst = (void *)d_sum;
  memsetParams.value = 0;
  memsetParams.pitch = 0;
  // elementSize can be max 4 bytes, so we take sizeof(float) and width=2
  memsetParams.elementSize = sizeof(float);
  memsetParams.width = 2;
  memsetParams.height = 1;

  /*
  DPCT1007:38: Migration of cudaGraphCreate is not supported.
  */
  checkCudaErrors(cudaGraphCreate(&graph, 0));
  /*
  DPCT1007:39: Migration of cudaGraphAddMemsetNode is not supported.
  */
  checkCudaErrors(
      cudaGraphAddMemsetNode(&memsetNode, graph, NULL, 0, &memsetParams));
  nodeDependencies.push_back(memsetNode);

  /*
  DPCT1082:40: Migration of cudaKernelNodeParams type is not supported.
  */
  cudaKernelNodeParams NodeParams0, NodeParams1;
  NodeParams0.func = (void *)JacobiMethod;
  NodeParams0.gridDim = nblocks;
  NodeParams0.blockDim = nthreads;
  NodeParams0.sharedMemBytes = 0;
  void *kernelArgs0[6] = {(void *)&A, (void *)&b,     (void *)&conv_threshold,
                          (void *)&x, (void *)&x_new, (void *)&d_sum};
  NodeParams0.kernelParams = kernelArgs0;
  NodeParams0.extra = NULL;

  /*
  DPCT1007:41: Migration of cudaGraphAddKernelNode is not supported.
  */
  checkCudaErrors(
      cudaGraphAddKernelNode(&jacobiKernelNode, graph, nodeDependencies.data(),
                             nodeDependencies.size(), &NodeParams0));

  nodeDependencies.clear();
  nodeDependencies.push_back(jacobiKernelNode);

  memcpyParams.from.image = NULL;
  memcpyParams.from.pos = sycl::id<3>(0, 0, 0);
  memcpyParams.from.pitched = dpct::pitched_data(d_sum, sizeof(double), 1, 1);
  memcpyParams.to.image = NULL;
  memcpyParams.to.pos = sycl::id<3>(0, 0, 0);
  memcpyParams.to.pitched = dpct::pitched_data(&sum, sizeof(double), 1, 1);
  memcpyParams.size = sycl::range<3>(sizeof(double), 1, 1);
  memcpyParams.direction = dpct::device_to_host;

  /*
  DPCT1007:42: Migration of cudaGraphAddMemcpyNode is not supported.
  */
  checkCudaErrors(
      cudaGraphAddMemcpyNode(&memcpyNode, graph, nodeDependencies.data(),
                             nodeDependencies.size(), &memcpyParams));

  /*
  DPCT1119:43: Migration of cudaGraphInstantiate is not supported, please try to
  remigrate with option: --use-experimental-features=graph.
  */
  checkCudaErrors(cudaGraphInstantiate(&graphExec, graph, NULL, NULL, 0));

  NodeParams1.func = (void *)JacobiMethod;
  NodeParams1.gridDim = nblocks;
  NodeParams1.blockDim = nthreads;
  NodeParams1.sharedMemBytes = 0;
  void *kernelArgs1[6] = {(void *)&A,     (void *)&b, (void *)&conv_threshold,
                          (void *)&x_new, (void *)&x, (void *)&d_sum};
  NodeParams1.kernelParams = kernelArgs1;
  NodeParams1.extra = NULL;

  int k = 0;
  for (k = 0; k < max_iter; k++) {
    /*
    DPCT1007:44: Migration of cudaGraphExecKernelNodeSetParams is not supported.
    */
    checkCudaErrors(cudaGraphExecKernelNodeSetParams(
        graphExec, jacobiKernelNode,
        ((k & 1) == 0) ? &NodeParams0 : &NodeParams1));
    /*
    DPCT1119:45: Migration of cudaGraphLaunch is not supported, please try to
    remigrate with option: --use-experimental-features=graph.
    */
    checkCudaErrors(cudaGraphLaunch(graphExec, stream));
    checkCudaErrors(DPCT_CHECK_ERROR(stream->wait()));

    if (sum <= conv_threshold) {
      checkCudaErrors(
          DPCT_CHECK_ERROR(stream->memset(d_sum, 0, sizeof(double))));
      nblocks.x = (N_ROWS / nthreads.x) + 1;
      /*
      DPCT1083:7: The size of local memory in the migrated code may be different
      from the original code. Check that the allocated memory size in the
      migrated code is correct.
      */
      size_t sharedMemSize = ((nthreads.x / 32) + 1) * sizeof(double);
      if ((k & 1) == 0) {
        /*
        DPCT1049:6: The work-group size passed to the SYCL kernel may exceed the
        limit. To get the device limit, query info::device::max_work_group_size.
        Adjust the work-group size if needed.
        */
        dpct::get_device(dpct::get_device_id(stream->get_device()))
            .has_capability_or_fail({sycl::aspect::fp64});

        stream->submit([&](sycl::handler &cgh) {
          sycl::local_accessor<uint8_t, 1> dpct_local_acc_ct1(
              sycl::range<1>(sharedMemSize), cgh);

          cgh.parallel_for(
              sycl::nd_range<3>(nblocks * nthreads, nthreads),
              [=](sycl::nd_item<3> item_ct1)
                  [[intel::reqd_sub_group_size(32)]] {
                    finalError(x_new, d_sum, item_ct1,
                               dpct_local_acc_ct1
                                   .get_multi_ptr<sycl::access::decorated::no>()
                                   .get());
                  });
        });
      } else {
        /*
        DPCT1049:8: The work-group size passed to the SYCL kernel may exceed the
        limit. To get the device limit, query info::device::max_work_group_size.
        Adjust the work-group size if needed.
        */
        dpct::get_device(dpct::get_device_id(stream->get_device()))
            .has_capability_or_fail({sycl::aspect::fp64});

        stream->submit([&](sycl::handler &cgh) {
          sycl::local_accessor<uint8_t, 1> dpct_local_acc_ct1(
              sycl::range<1>(sharedMemSize), cgh);

          cgh.parallel_for(
              sycl::nd_range<3>(nblocks * nthreads, nthreads),
              [=](sycl::nd_item<3> item_ct1)
                  [[intel::reqd_sub_group_size(32)]] {
                    finalError(x, d_sum, item_ct1,
                               dpct_local_acc_ct1
                                   .get_multi_ptr<sycl::access::decorated::no>()
                                   .get());
                  });
        });
      }

      /*
      DPCT1124:46: cudaMemcpyAsync is migrated to asynchronous memcpy API. While
      the origin API might be synchronous, it depends on the type of operand
      memory, so you may need to call wait() on event return by memcpy API to
      ensure synchronization behavior.
      */
      checkCudaErrors(
          DPCT_CHECK_ERROR(stream->memcpy(&sum, d_sum, sizeof(double))));
      checkCudaErrors(DPCT_CHECK_ERROR(stream->wait()));
      printf("GPU iterations : %d\n", k + 1);
      printf("GPU error : %.3e\n", sum);
      break;
    }
  }

  checkCudaErrors(
      DPCT_CHECK_ERROR(dpct::dpct_free(d_sum, dpct::get_in_order_queue())));
  return sum;
}

double JacobiMethodGpuCudaGraphExecUpdate(const float *A, const double *b,
                                          const float conv_threshold,
                                          const int max_iter, double *x,
                                          double *x_new,
                                          dpct::queue_ptr stream) {
  // CTA size
  dpct::dim3 nthreads(256, 1, 1);
  // grid size
  dpct::dim3 nblocks((N_ROWS / ROWS_PER_CTA) + 2, 1, 1);
  /*
  DPCT1119:47: Migration of cudaGraph_t is not supported, please try to
  remigrate with option: --use-experimental-features=graph.
  */
  cudaGraph_t graph;
  /*
  DPCT1119:48: Migration of cudaGraphExec_t is not supported, please try to
  remigrate with option: --use-experimental-features=graph.
  */
  cudaGraphExec_t graphExec = NULL;

  double sum = 0.0;
  double *d_sum;
  checkCudaErrors(DPCT_CHECK_ERROR(
      d_sum = sycl::malloc_device<double>(1, dpct::get_in_order_queue())));

  int k = 0;
  for (k = 0; k < max_iter; k++) {
    /*
    DPCT1119:49: Migration of cudaStreamBeginCapture is not supported, please
    try to remigrate with option: --use-experimental-features=graph.
    */
    checkCudaErrors(
        cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal));
    checkCudaErrors(DPCT_CHECK_ERROR(stream->memset(d_sum, 0, sizeof(double))));
    if ((k & 1) == 0) {
      /*
      DPCT1049:9: The work-group size passed to the SYCL kernel may exceed the
      limit. To get the device limit, query info::device::max_work_group_size.
      Adjust the work-group size if needed.
      */
      dpct::get_device(dpct::get_device_id(stream->get_device()))
          .has_capability_or_fail({sycl::aspect::fp64});

      stream->submit([&](sycl::handler &cgh) {
        /*
        DPCT1101:64: 'N_ROWS' expression was replaced with a value. Modify
        the code to use the original expression, provided in comments, if it
        is correct.
        */
        sycl::local_accessor<double, 1> x_shared_acc_ct1(
            sycl::range<1>(512 /*N_ROWS*/), cgh);
        /*
        DPCT1101:65: 'ROWS_PER_CTA + 1' expression was replaced with a
        value. Modify the code to use the original expression, provided in
        comments, if it is correct.
        */
        sycl::local_accessor<double, 1> b_shared_acc_ct1(
            sycl::range<1>(9 /*ROWS_PER_CTA + 1*/), cgh);

        cgh.parallel_for(
            sycl::nd_range<3>(nblocks * nthreads, nthreads),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
              JacobiMethod(
                  A, b, conv_threshold, x, x_new, d_sum, item_ct1,
                  x_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get(),
                  b_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get());
            });
      });
    } else {
      /*
      DPCT1049:10: The work-group size passed to the SYCL kernel may exceed the
      limit. To get the device limit, query info::device::max_work_group_size.
      Adjust the work-group size if needed.
      */
      dpct::get_device(dpct::get_device_id(stream->get_device()))
          .has_capability_or_fail({sycl::aspect::fp64});

      stream->submit([&](sycl::handler &cgh) {
        /*
        DPCT1101:66: 'N_ROWS' expression was replaced with a value. Modify
        the code to use the original expression, provided in comments, if it
        is correct.
        */
        sycl::local_accessor<double, 1> x_shared_acc_ct1(
            sycl::range<1>(512 /*N_ROWS*/), cgh);
        /*
        DPCT1101:67: 'ROWS_PER_CTA + 1' expression was replaced with a
        value. Modify the code to use the original expression, provided in
        comments, if it is correct.
        */
        sycl::local_accessor<double, 1> b_shared_acc_ct1(
            sycl::range<1>(9 /*ROWS_PER_CTA + 1*/), cgh);

        cgh.parallel_for(
            sycl::nd_range<3>(nblocks * nthreads, nthreads),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
              JacobiMethod(
                  A, b, conv_threshold, x_new, x, d_sum, item_ct1,
                  x_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get(),
                  b_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get());
            });
      });
    }
    /*
    DPCT1124:50: cudaMemcpyAsync is migrated to asynchronous memcpy API. While
    the origin API might be synchronous, it depends on the type of operand
    memory, so you may need to call wait() on event return by memcpy API to
    ensure synchronization behavior.
    */
    checkCudaErrors(
        DPCT_CHECK_ERROR(stream->memcpy(&sum, d_sum, sizeof(double))));
    /*
    DPCT1119:51: Migration of cudaStreamEndCapture is not supported, please try
    to remigrate with option: --use-experimental-features=graph.
    */
    checkCudaErrors(cudaStreamEndCapture(stream, &graph));

    if (graphExec == NULL) {
      /*
      DPCT1119:52: Migration of cudaGraphInstantiate is not supported, please
      try to remigrate with option: --use-experimental-features=graph.
      */
      checkCudaErrors(cudaGraphInstantiate(&graphExec, graph, NULL, NULL, 0));
    } else {
      /*
      DPCT1007:53: Migration of cudaGraphExecUpdateResult is not supported.
      */
      cudaGraphExecUpdateResult updateResult_out;
      /*
      DPCT1119:54: Migration of cudaGraphExecUpdate is not supported, please try
      to remigrate with option: --use-experimental-features=graph.
      */
      checkCudaErrors(
          cudaGraphExecUpdate(graphExec, graph, NULL, &updateResult_out));
      if (updateResult_out != cudaGraphExecUpdateSuccess) {
        if (graphExec != NULL) {
          /*
          DPCT1119:55: Migration of cudaGraphExecDestroy is not supported,
          please try to remigrate with option:
          --use-experimental-features=graph.
          */
          checkCudaErrors(cudaGraphExecDestroy(graphExec));
        }
        printf("k = %d graph update failed with error - %d\n", k,
               updateResult_out);
        /*
        DPCT1119:56: Migration of cudaGraphInstantiate is not supported, please
        try to remigrate with option: --use-experimental-features=graph.
        */
        checkCudaErrors(cudaGraphInstantiate(&graphExec, graph, NULL, NULL, 0));
      }
    }
    /*
    DPCT1119:57: Migration of cudaGraphLaunch is not supported, please try to
    remigrate with option: --use-experimental-features=graph.
    */
    checkCudaErrors(cudaGraphLaunch(graphExec, stream));
    checkCudaErrors(DPCT_CHECK_ERROR(stream->wait()));

    if (sum <= conv_threshold) {
      checkCudaErrors(
          DPCT_CHECK_ERROR(stream->memset(d_sum, 0, sizeof(double))));
      nblocks.x = (N_ROWS / nthreads.x) + 1;
      /*
      DPCT1083:12: The size of local memory in the migrated code may be
      different from the original code. Check that the allocated memory size in
      the migrated code is correct.
      */
      size_t sharedMemSize = ((nthreads.x / 32) + 1) * sizeof(double);
      if ((k & 1) == 0) {
        /*
        DPCT1049:11: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::get_device(dpct::get_device_id(stream->get_device()))
            .has_capability_or_fail({sycl::aspect::fp64});

        stream->submit([&](sycl::handler &cgh) {
          sycl::local_accessor<uint8_t, 1> dpct_local_acc_ct1(
              sycl::range<1>(sharedMemSize), cgh);

          cgh.parallel_for(
              sycl::nd_range<3>(nblocks * nthreads, nthreads),
              [=](sycl::nd_item<3> item_ct1)
                  [[intel::reqd_sub_group_size(32)]] {
                    finalError(x_new, d_sum, item_ct1,
                               dpct_local_acc_ct1
                                   .get_multi_ptr<sycl::access::decorated::no>()
                                   .get());
                  });
        });
      } else {
        /*
        DPCT1049:13: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::get_device(dpct::get_device_id(stream->get_device()))
            .has_capability_or_fail({sycl::aspect::fp64});

        stream->submit([&](sycl::handler &cgh) {
          sycl::local_accessor<uint8_t, 1> dpct_local_acc_ct1(
              sycl::range<1>(sharedMemSize), cgh);

          cgh.parallel_for(
              sycl::nd_range<3>(nblocks * nthreads, nthreads),
              [=](sycl::nd_item<3> item_ct1)
                  [[intel::reqd_sub_group_size(32)]] {
                    finalError(x, d_sum, item_ct1,
                               dpct_local_acc_ct1
                                   .get_multi_ptr<sycl::access::decorated::no>()
                                   .get());
                  });
        });
      }

      /*
      DPCT1124:58: cudaMemcpyAsync is migrated to asynchronous memcpy API. While
      the origin API might be synchronous, it depends on the type of operand
      memory, so you may need to call wait() on event return by memcpy API to
      ensure synchronization behavior.
      */
      checkCudaErrors(
          DPCT_CHECK_ERROR(stream->memcpy(&sum, d_sum, sizeof(double))));
      checkCudaErrors(DPCT_CHECK_ERROR(stream->wait()));
      printf("GPU iterations : %d\n", k + 1);
      printf("GPU error : %.3e\n", sum);
      break;
    }
  }

  checkCudaErrors(
      DPCT_CHECK_ERROR(dpct::dpct_free(d_sum, dpct::get_in_order_queue())));
  return sum;
}

double JacobiMethodGpu(const float *A, const double *b,
                       const float conv_threshold, const int max_iter,
                       double *x, double *x_new, dpct::queue_ptr stream) {
  // CTA size
  dpct::dim3 nthreads(256, 1, 1);
  // grid size
  dpct::dim3 nblocks((N_ROWS / ROWS_PER_CTA) + 2, 1, 1);

  double sum = 0.0;
  double *d_sum;
  checkCudaErrors(DPCT_CHECK_ERROR(
      d_sum = sycl::malloc_device<double>(1, dpct::get_in_order_queue())));
  int k = 0;

  for (k = 0; k < max_iter; k++) {
    checkCudaErrors(DPCT_CHECK_ERROR(stream->memset(d_sum, 0, sizeof(double))));
    if ((k & 1) == 0) {
      /*
      DPCT1049:14: The work-group size passed to the SYCL kernel may exceed the
      limit. To get the device limit, query info::device::max_work_group_size.
      Adjust the work-group size if needed.
      */
      dpct::get_device(dpct::get_device_id(stream->get_device()))
          .has_capability_or_fail({sycl::aspect::fp64});

      stream->submit([&](sycl::handler &cgh) {
        /*
        DPCT1101:68: 'N_ROWS' expression was replaced with a value. Modify
        the code to use the original expression, provided in comments, if it
        is correct.
        */
        sycl::local_accessor<double, 1> x_shared_acc_ct1(
            sycl::range<1>(512 /*N_ROWS*/), cgh);
        /*
        DPCT1101:69: 'ROWS_PER_CTA + 1' expression was replaced with a
        value. Modify the code to use the original expression, provided in
        comments, if it is correct.
        */
        sycl::local_accessor<double, 1> b_shared_acc_ct1(
            sycl::range<1>(9 /*ROWS_PER_CTA + 1*/), cgh);

        cgh.parallel_for(
            sycl::nd_range<3>(nblocks * nthreads, nthreads),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
              JacobiMethod(
                  A, b, conv_threshold, x, x_new, d_sum, item_ct1,
                  x_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get(),
                  b_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get());
            });
      });
    } else {
      /*
      DPCT1049:15: The work-group size passed to the SYCL kernel may exceed the
      limit. To get the device limit, query info::device::max_work_group_size.
      Adjust the work-group size if needed.
      */
      dpct::get_device(dpct::get_device_id(stream->get_device()))
          .has_capability_or_fail({sycl::aspect::fp64});

      stream->submit([&](sycl::handler &cgh) {
        /*
        DPCT1101:70: 'N_ROWS' expression was replaced with a value. Modify
        the code to use the original expression, provided in comments, if it
        is correct.
        */
        sycl::local_accessor<double, 1> x_shared_acc_ct1(
            sycl::range<1>(512 /*N_ROWS*/), cgh);
        /*
        DPCT1101:71: 'ROWS_PER_CTA + 1' expression was replaced with a
        value. Modify the code to use the original expression, provided in
        comments, if it is correct.
        */
        sycl::local_accessor<double, 1> b_shared_acc_ct1(
            sycl::range<1>(9 /*ROWS_PER_CTA + 1*/), cgh);

        cgh.parallel_for(
            sycl::nd_range<3>(nblocks * nthreads, nthreads),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
              JacobiMethod(
                  A, b, conv_threshold, x_new, x, d_sum, item_ct1,
                  x_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get(),
                  b_shared_acc_ct1.get_multi_ptr<sycl::access::decorated::no>()
                      .get());
            });
      });
    }
    /*
    DPCT1124:59: cudaMemcpyAsync is migrated to asynchronous memcpy API. While
    the origin API might be synchronous, it depends on the type of operand
    memory, so you may need to call wait() on event return by memcpy API to
    ensure synchronization behavior.
    */
    checkCudaErrors(
        DPCT_CHECK_ERROR(stream->memcpy(&sum, d_sum, sizeof(double))));
    checkCudaErrors(DPCT_CHECK_ERROR(stream->wait()));

    if (sum <= conv_threshold) {
      checkCudaErrors(
          DPCT_CHECK_ERROR(stream->memset(d_sum, 0, sizeof(double))));
      nblocks.x = (N_ROWS / nthreads.x) + 1;
      /*
      DPCT1083:17: The size of local memory in the migrated code may be
      different from the original code. Check that the allocated memory size in
      the migrated code is correct.
      */
      size_t sharedMemSize = ((nthreads.x / 32) + 1) * sizeof(double);
      if ((k & 1) == 0) {
        /*
        DPCT1049:16: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::get_device(dpct::get_device_id(stream->get_device()))
            .has_capability_or_fail({sycl::aspect::fp64});

        stream->submit([&](sycl::handler &cgh) {
          sycl::local_accessor<uint8_t, 1> dpct_local_acc_ct1(
              sycl::range<1>(sharedMemSize), cgh);

          cgh.parallel_for(
              sycl::nd_range<3>(nblocks * nthreads, nthreads),
              [=](sycl::nd_item<3> item_ct1)
                  [[intel::reqd_sub_group_size(32)]] {
                    finalError(x_new, d_sum, item_ct1,
                               dpct_local_acc_ct1
                                   .get_multi_ptr<sycl::access::decorated::no>()
                                   .get());
                  });
        });
      } else {
        /*
        DPCT1049:18: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::get_device(dpct::get_device_id(stream->get_device()))
            .has_capability_or_fail({sycl::aspect::fp64});

        stream->submit([&](sycl::handler &cgh) {
          sycl::local_accessor<uint8_t, 1> dpct_local_acc_ct1(
              sycl::range<1>(sharedMemSize), cgh);

          cgh.parallel_for(
              sycl::nd_range<3>(nblocks * nthreads, nthreads),
              [=](sycl::nd_item<3> item_ct1)
                  [[intel::reqd_sub_group_size(32)]] {
                    finalError(x, d_sum, item_ct1,
                               dpct_local_acc_ct1
                                   .get_multi_ptr<sycl::access::decorated::no>()
                                   .get());
                  });
        });
      }

      /*
      DPCT1124:60: cudaMemcpyAsync is migrated to asynchronous memcpy API. While
      the origin API might be synchronous, it depends on the type of operand
      memory, so you may need to call wait() on event return by memcpy API to
      ensure synchronization behavior.
      */
      checkCudaErrors(
          DPCT_CHECK_ERROR(stream->memcpy(&sum, d_sum, sizeof(double))));
      checkCudaErrors(DPCT_CHECK_ERROR(stream->wait()));
      printf("GPU iterations : %d\n", k + 1);
      printf("GPU error : %.3e\n", sum);
      break;
    }
  }

  checkCudaErrors(
      DPCT_CHECK_ERROR(dpct::dpct_free(d_sum, dpct::get_in_order_queue())));
  return sum;
}
