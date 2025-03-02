// Copyright © 2023-2024 Apple Inc.

#include "mlx/allocator.h"
#include "mlx/backend/common/copy.h"
#include "mlx/backend/common/lapack.h"
#include "mlx/primitives.h"

namespace mlx::core {

template <typename T>
struct lpack;

template <>
struct lpack<float> {
  static void xgeqrf(
      const int* m,
      const int* n,
      float* a,
      const int* lda,
      float* tau,
      float* work,
      const int* lwork,
      int* info) {
    sgeqrf_(m, n, a, lda, tau, work, lwork, info);
  }
  static void xorgqr(
      const int* m,
      const int* n,
      const int* k,
      float* a,
      const int* lda,
      const float* tau,
      float* work,
      const int* lwork,
      int* info) {
    sorgqr_(m, n, k, a, lda, tau, work, lwork, info);
  }
};

template <typename T>
void qrf_impl(const array& a, array& q, array& r) {
  const int M = a.shape(-2);
  const int N = a.shape(-1);
  const int lda = M;
  size_t num_matrices = a.size() / (M * N);
  int num_reflectors = std::min(M, N);
  auto tau =
      allocator::malloc_or_wait(sizeof(T) * num_matrices * num_reflectors);

  // Copy A to inplace input and make it col-contiguous
  array in(a.shape(), float32, nullptr, {});
  auto flags = in.flags();

  // Copy the input to be column contiguous
  flags.col_contiguous = num_matrices == 1;
  flags.row_contiguous = false;
  auto strides = in.strides();
  strides[in.ndim() - 2] = 1;
  strides[in.ndim() - 1] = M;
  in.set_data(
      allocator::malloc_or_wait(in.nbytes()), in.nbytes(), strides, flags);
  copy_inplace(a, in, CopyType::GeneralGeneral);

  T optimal_work;
  int lwork = -1;
  int info;

  // Compute workspace size
  lpack<T>::xgeqrf(
      &M, &N, nullptr, &lda, nullptr, &optimal_work, &lwork, &info);

  // Update workspace size
  lwork = optimal_work;
  auto work = allocator::malloc_or_wait(sizeof(T) * lwork);

  // Loop over matrices
  for (int i = 0; i < num_matrices; ++i) {
    // Solve
    lpack<T>::xgeqrf(
        &M,
        &N,
        in.data<float>() + M * N * i,
        &lda,
        static_cast<T*>(tau.raw_ptr()) + num_reflectors * i,
        static_cast<T*>(work.raw_ptr()),
        &lwork,
        &info);
  }
  allocator::free(work);

  r.set_data(allocator::malloc_or_wait(r.nbytes()));

  for (int i = 0; i < num_matrices; ++i) {
    /// num_reflectors x N
    for (int j = 0; j < r.shape(-2); ++j) {
      for (int k = 0; k < j; ++k) {
        r.data<T>()[i * N * num_reflectors + j * N + k] = 0;
      }
      for (int k = j; k < r.shape(-1); ++k) {
        r.data<T>()[i * N * num_reflectors + j * N + k] =
            in.data<T>()[i * N * M + j + k * M];
      }
    }
  }

  // Get work size
  lwork = -1;
  lpack<T>::xorgqr(
      &M,
      &num_reflectors,
      &num_reflectors,
      nullptr,
      &lda,
      nullptr,
      &optimal_work,
      &lwork,
      &info);
  lwork = optimal_work;
  work = allocator::malloc_or_wait(sizeof(T) * lwork);

  // Loop over matrices
  for (int i = 0; i < num_matrices; ++i) {
    // Compute Q
    lpack<T>::xorgqr(
        &M,
        &num_reflectors,
        &num_reflectors,
        in.data<float>() + M * N * i,
        &lda,
        static_cast<T*>(tau.raw_ptr()) + num_reflectors * i,
        static_cast<T*>(work.raw_ptr()),
        &lwork,
        &info);
  }

  q.set_data(allocator::malloc_or_wait(q.nbytes()));
  for (int i = 0; i < num_matrices; ++i) {
    // M x num_reflectors
    for (int j = 0; j < q.shape(-2); ++j) {
      for (int k = 0; k < q.shape(-1); ++k) {
        q.data<T>()[i * M * num_reflectors + j * num_reflectors + k] =
            in.data<T>()[i * N * M + j + k * M];
      }
    }
  }

  // Cleanup
  allocator::free(work);
  allocator::free(tau);
}

void QRF::eval_cpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  if (!(inputs[0].dtype() == float32)) {
    throw std::runtime_error("[QRF::eval] only supports float32.");
  }
  qrf_impl<float>(inputs[0], outputs[0], outputs[1]);
}

} // namespace mlx::core
