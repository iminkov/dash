#ifndef DASH__ALGORITHM__SUMMA_H_
#define DASH__ALGORITHM__SUMMA_H_

#include <dash/Exception.h>
// #include <dash/bindings/LAPACK.h>

#include <utility>

namespace dash {

namespace internal {

/**
 * Naive matrix multiplication for local multiplication of matrix blocks,
 * used only for tests and where BLAS is not available.
 */
template<
  typename MatrixTypeA,
  typename MatrixTypeB,
  typename MatrixTypeC
>
void multiply_naive(
  /// Matrix to multiply, extents n x m
  MatrixTypeA & A,
  /// Matrix to multiply, extents m x p
  MatrixTypeB & B,
  /// Matrix to contain the multiplication result, extents n x p,
  /// initialized with zeros
  MatrixTypeC & C,
  size_t        m,
  size_t        n,
  size_t        p)
{
  typedef typename MatrixTypeC::pattern_type pattern_c_type;
  typedef typename MatrixTypeC::value_type   value_type;
  for (auto i = 0; i < n; ++i) {
    // row i = 0...n
    for (auto j = 0; j < p; ++j) {
      // column j = 0...p
      value_type c_sum = C[j][i];
      for (auto k = 0; k < m; ++k) {
        // k = 0...m
        auto ik    = i * m + k;
        auto kj    = k * m + j;
        auto value = A[ik] * B[kj];
        DASH_LOG_TRACE("dash::internal::multiply_naive", "summa.multiply",
                       "C(", j, ",", i, ") +=",
                       "A[", ik, "] * B[", kj, "] = ",
                       A[ik], "*", B[kj], "=", value);
        c_sum += value;
      }
      C[j][i] = c_sum;
    }
  }
} 

} // namespace internal

/// Constraints on pattern partitioning properties of matrix operands passed to
/// \c dash::summa.
typedef dash::pattern_partitioning_properties<
          // same number of elements in every block
          pattern_partitioning_tag::balanced >
        summa_pattern_partitioning_constraints;
/// Constraints on pattern mapping properties of matrix operands passed to
/// \c dash::summa.
typedef dash::pattern_mapping_properties<
          // same amount of blocks for every process
          pattern_mapping_tag::balanced,
          // every process mapped in every row/column
          pattern_mapping_tag::diagonal >
        summa_pattern_mapping_constraints;
/// Constraints on pattern layout properties of matrix operands passed to
/// \c dash::summa.
typedef dash::pattern_layout_properties<
          // elements contiguous within block
          pattern_layout_tag::local_phase >
        summa_pattern_layout_constraints;

/**
 * Multiplies two matrices using the SUMMA algorithm.
 *
 * Pseudocode:
 *
 *   C = zeros(n,n)
 *   for k = 1:b:n {            // k increments in steps of blocksize b
 *     u = k:(k+b-1)            // u is [k, k+1, ..., k+b-1]
 *     C = C + A(:,u) * B(u,:)  // Multiply n x b matrix from A with
 *                              // b x p matrix from B
 *   }
 */
template<
  typename MatrixTypeA,
  typename MatrixTypeB,
  typename MatrixTypeC
>
void summa(
  /// Matrix to multiply, extents n x m
  MatrixTypeA & A,
  /// Matrix to multiply, extents m x p
  MatrixTypeB & B,
  /// Matrix to contain the multiplication result, extents n x p,
  /// initialized with zeros
  MatrixTypeC & C)
{
  typedef typename MatrixTypeA::value_type   value_type;
  typedef typename MatrixTypeA::index_type   index_t;
  typedef typename MatrixTypeA::pattern_type pattern_a_type;
  typedef typename MatrixTypeB::pattern_type pattern_b_type;
  typedef typename MatrixTypeC::pattern_type pattern_c_type;
  typedef std::array<index_t, 2>             coords_t;

  DASH_LOG_DEBUG("dash::summa()");
  // Verify that matrix patterns satisfy pattern constraints:
  if (!dash::check_pattern_constraints<
         summa_pattern_partitioning_constraints,
         summa_pattern_mapping_constraints,
         summa_pattern_layout_constraints
       >(A.pattern())) {
    DASH_THROW(
      dash::exception::InvalidArgument,
      "dash::summa(): "
      "pattern of first matrix argument does not match constraints");
  }
  if (!dash::check_pattern_constraints<
         summa_pattern_partitioning_constraints,
         summa_pattern_mapping_constraints,
         summa_pattern_layout_constraints
       >(B.pattern())) {
    DASH_THROW(
      dash::exception::InvalidArgument,
      "dash::summa(): "
      "pattern of second matrix argument does not match constraints");
  }
  if (!dash::check_pattern_constraints<
         summa_pattern_partitioning_constraints,
         summa_pattern_mapping_constraints,
         summa_pattern_layout_constraints
       >(C.pattern())) {
    DASH_THROW(
      dash::exception::InvalidArgument,
      "dash::summa(): "
      "pattern of result matrix does not match constraints");
  }
  DASH_LOG_TRACE("dash::summa", "matrix pattern properties valid");

  //    A         B         C
  //  _____     _____     _____
  // |     |   |     |   |     |
  // n     | x m     | = n     |
  // |_ m _|   |_ p _|   |_ p _|
  //
  dash::Team & team = C.team();
  auto unit_id      = team.myid();
  // Check run-time invariants on pattern instances:
  auto pattern_a    = A.pattern();
  auto pattern_b    = B.pattern();
  auto pattern_c    = C.pattern();
  auto m = pattern_a.extent(0); // number of columns in A, rows in B
  auto n = pattern_a.extent(1); // number of rows in A and C
  auto p = pattern_b.extent(0); // number of columns in B and C

  DASH_ASSERT_EQ(
    pattern_a.extent(1),
    pattern_b.extent(0),
    "dash::summa(): "
    "Extents of first operand in dimension 1 do not match extents of"
    "second operand in dimension 0");
  DASH_ASSERT_EQ(
    pattern_c.extent(0),
    pattern_a.extent(0),
    "dash::summa(): "
    "Extents of result matrix in dimension 0 do not match extents of"
    "first operand in dimension 0");
  DASH_ASSERT_EQ(
    pattern_c.extent(1),
    pattern_b.extent(1),
    "dash::summa(): "
    "Extents of result matrix in dimension 1 do not match extents of"
    "second operand in dimension 1");

  DASH_LOG_TRACE("dash::summa", "matrix pattern extents valid");

  // Patterns are balanced, all blocks have identical size:
  auto block_size_m  = pattern_a.block(0).extent(0);
  auto block_size_n  = pattern_b.block(0).extent(1);
  auto block_size_p  = pattern_b.block(0).extent(0);
  auto num_blocks_l  = n / block_size_n;
  auto num_blocks_m  = m / block_size_m;
  auto num_blocks_n  = n / block_size_n;
  auto num_blocks_p  = p / block_size_p;
  // Size of temporary local blocks
  auto block_a_size  = block_size_n * block_size_m;
  auto block_b_size  = block_size_m * block_size_p;
  // Number of units in rows and columns:
  auto teamspec      = C.pattern().teamspec();
  auto num_units_x   = teamspec.extent(0);
  auto num_units_y   = teamspec.extent(1);
  // Coordinates of active unit in team spec (process grid):
  auto team_coords_u = teamspec.coords(unit_id);
  // Block row and column in C assigned to active unit:
  auto block_col_u   = team_coords_u[0];
  auto block_row_u   = team_coords_u[1];

  DASH_LOG_TRACE("dash::summa", "blocks:",
                 "m:", num_blocks_m, "*", block_size_m,
                 "n:", num_blocks_n, "*", block_size_n,
                 "p:", num_blocks_p, "*", block_size_p);
  DASH_LOG_TRACE("dash::summa", "number of units:",
                 "cols:", num_units_x,
                 "rows:", num_units_y);
  DASH_LOG_TRACE("dash::summa", "allocating local temporary blocks, sizes:",
                 "A:", block_a_size,
                 "B:", block_b_size);
  value_type * local_block_a_get  = new value_type[block_a_size];
  value_type * local_block_b_get  = new value_type[block_b_size];
  value_type * local_block_a_comp = new value_type[block_a_size];
  value_type * local_block_b_comp = new value_type[block_b_size];

  // Pre-fetch first blocks in A and B:
  auto l_block_c_get         = C.local.block(0);
  auto l_block_c_get_view    = l_block_c_get.begin().viewspec();
  // Block coordinate of current local block in matrix C:
  index_t l_block_c_get_row  = l_block_c_get_view.offset(1) / block_size_n;
  index_t l_block_c_get_col  = l_block_c_get_view.offset(0) / block_size_p;
  auto l_block_c_comp        = l_block_c_get;
  auto l_block_c_comp_view   = l_block_c_comp.begin().viewspec();
  index_t l_block_c_comp_row = l_block_c_comp_view.offset(1) / block_size_n;
  index_t l_block_c_comp_col = l_block_c_comp_view.offset(0) / block_size_p;
  auto block_a = A.block(coords_t { 0, l_block_c_get_row });
  auto block_b = B.block(coords_t { l_block_c_get_col, 0 });
  DASH_LOG_TRACE("dash::summa", "summa.block",
                 "prefetching local copy of A.block:",
                 "col:",  0,
                 "row:",  l_block_c_get_row,
                 "view:", block_a.begin().viewspec());
  auto get_a = dash::copy_async(block_a.begin(), block_a.end(),
                                local_block_a_comp);
  DASH_LOG_TRACE("dash::summa", "summa.block",
                 "prefetching local copy of B.block:",
                 "col:",  l_block_c_get_col,
                 "row:",  0,
                 "view:", block_b.begin().viewspec());
  auto get_b = dash::copy_async(block_b.begin(), block_b.end(),
                                local_block_b_comp);
  DASH_LOG_TRACE("dash::summa", "summa.block",
                 "waiting for prefetching of blocks");
  get_a.wait();
  get_b.wait();
  DASH_LOG_TRACE("dash::summa", "summa.block",
                 "prefetching of blocks completed");
  // Iterate local blocks in matrix C:
  //
  auto num_local_blocks_c = (num_blocks_n * num_blocks_p) / teamspec.size();
  for (auto lb = 0; lb < num_local_blocks_c; ++lb) {
    // Block coordinates for next block multiplication result:
    l_block_c_comp      = C.local.block(lb);
    l_block_c_comp_view = l_block_c_comp.begin().viewspec();
    l_block_c_comp_row  = l_block_c_comp_view.offset(1) / block_size_n;
    l_block_c_comp_col  = l_block_c_comp_view.offset(0) / block_size_p;
    l_block_c_get       = l_block_c_comp;
    l_block_c_get_view  = l_block_c_comp_view;
    l_block_c_get_row   = l_block_c_get_row;
    l_block_c_get_col   = l_block_c_get_col;
    DASH_LOG_TRACE("dash::summa", "summa.block.c", "C.local.block.comp", lb,
                   "row:",  l_block_c_comp_row,
                   "col:",  l_block_c_comp_col,
                   "view:", l_block_c_comp_view);
    // Iterate blocks in columns of A / rows of B:
    //
    for (index_t block_k = 0; block_k < num_blocks_m; ++block_k) {
      // Do not prefetch blocks in last iteration:
      bool last = (lb == num_local_blocks_c - 1) && 
                  (block_k == num_blocks_m - 1);
      if (!last) {
        auto block_get_k = block_k + 1;
        // Block coordinate of local block in matrix C to prefetch:
        if (block_k == num_blocks_m - 1) {
          // Prefetch for next local block in matrix C:
          block_get_k        = 0;
          l_block_c_get      = C.local.block(lb + 1);
          l_block_c_get_view = l_block_c_get.begin().viewspec();
          l_block_c_get_row  = l_block_c_get_view.offset(1) / block_size_n;
          l_block_c_get_col  = l_block_c_get_view.offset(0) / block_size_p;
        }
        // Async request for local copy of blocks from A and B:
        //
        block_a = A.block(coords_t { block_get_k, l_block_c_get_row });
        DASH_LOG_TRACE("dash::summa", "summa.block.a",
                       "requesting local copy of A.block:",
                       "col:",  block_get_k,
                       "row:",  l_block_c_get_row,
                       "view:", block_a.begin().viewspec());
        get_a = dash::copy_async(block_a.begin(), block_a.end(),
                                 local_block_a_get);
        block_b = B.block(coords_t { l_block_c_get_col, block_get_k });
        DASH_LOG_TRACE("dash::summa", "summa.block.b",
                       "requesting local copy of B.block:",
                       "col:",  l_block_c_get_col,
                       "row:",  block_get_k,
                       "view:", block_b.begin().viewspec());
        get_b = dash::copy_async(block_b.begin(), block_b.end(),
                                 local_block_b_get);
      } else {
        DASH_LOG_TRACE("dash::summa", " ->",
                       "last block multiplication",
                       "lb:", lb, "bk:", block_k);
      }
      // Computation of matrix product of local block matrices:
      //
      DASH_LOG_TRACE("dash::summa", " ->",
                     "multiplying local block matrices",
                     "C.local.block.comp:", lb,
                     "view:", l_block_c_comp.begin().viewspec());
      dash::internal::multiply_naive(
          local_block_a_comp,
          local_block_b_comp,
          l_block_c_comp,
          block_size_m,
          block_size_n,
          block_size_p);
      if (!last) {
        // Wait for local copies:
        //
        DASH_LOG_TRACE("dash::summa", " ->",
                       "waiting for local copies of next blocks");
        get_a.wait();
        get_b.wait();
        DASH_LOG_TRACE("dash::summa", " ->",
                       "local copies of next blocks received");
        // Swap communication and computation buffers:
        //
        std::swap(local_block_a_get, local_block_a_comp);
        std::swap(local_block_b_get, local_block_b_comp);
      }
    }
  } // for lb

  delete[] local_block_a_get;
  delete[] local_block_b_get;
  delete[] local_block_a_comp;
  delete[] local_block_b_comp;

  C.barrier();
}

/**
 * Registration of \c dash::summa as an implementation of matrix-matrix
 * multiplication (xDGEMM).
 *
 * Delegates  \c dash::multiply<MatrixType>
 * to         \c dash::summa<MatrixType>
 * if         \c MatrixType::pattern_type
 * satisfies the pattern property constraints of the SUMMA implementation.
 */
template<
  typename MatrixTypeA,
  typename MatrixTypeB,
  typename MatrixTypeC
>
typename std::enable_if<
  dash::pattern_constraints<
    dash::summa_pattern_partitioning_constraints,
    dash::summa_pattern_mapping_constraints,
    dash::summa_pattern_layout_constraints,
    typename MatrixTypeA::pattern_type
  >::satisfied::value &&
  dash::pattern_constraints<
    dash::summa_pattern_partitioning_constraints,
    dash::summa_pattern_mapping_constraints,
    dash::summa_pattern_layout_constraints,
    typename MatrixTypeB::pattern_type
  >::satisfied::value &&
  dash::pattern_constraints<
    dash::summa_pattern_partitioning_constraints,
    dash::summa_pattern_mapping_constraints,
    dash::summa_pattern_layout_constraints,
    typename MatrixTypeC::pattern_type
  >::satisfied::value,
  void
>::type
multiply(
  /// Matrix to multiply, extents n x m
  MatrixTypeA & A,
  /// Matrix to multiply, extents m x p
  MatrixTypeB & B,
  /// Matrix to contain the multiplication result, extents n x p,
  /// initialized with zeros
  MatrixTypeC & C)
{
  dash::summa(A, B, C);
}

} // namespace dash

#endif
