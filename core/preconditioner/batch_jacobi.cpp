// SPDX-FileCopyrightText: 2017 - 2024 The Ginkgo authors
//
// SPDX-License-Identifier: BSD-3-Clause

#include <ginkgo/core/preconditioner/batch_jacobi.hpp>


#include "core/matrix/batch_csr_kernels.hpp"
#include "core/matrix/csr_kernels.hpp"
#include "core/preconditioner/batch_jacobi_kernels.hpp"
#include "core/preconditioner/jacobi_kernels.hpp"


namespace gko {
namespace batch {
namespace preconditioner {
namespace jacobi {


GKO_REGISTER_OPERATION(find_blocks, jacobi::find_blocks);
GKO_REGISTER_OPERATION(extract_common_blocks_pattern,
                       batch_jacobi::extract_common_blocks_pattern);
GKO_REGISTER_OPERATION(compute_block_jacobi,
                       batch_jacobi::compute_block_jacobi);
GKO_REGISTER_OPERATION(find_row_is_part_of_which_block,
                       batch_jacobi::find_row_is_part_of_which_block);
GKO_REGISTER_OPERATION(compute_cumulative_block_storage,
                       batch_jacobi::compute_cumulative_block_storage);


}  // namespace jacobi


template <typename ValueType, typename IndexType>
void Jacobi<ValueType, IndexType>::detect_blocks(
    const size_type num_batch,
    const gko::matrix::Csr<ValueType, IndexType>* first_system)
{
    parameters_.block_pointers.resize_and_reset(first_system->get_size()[0] +
                                                1);
    this->get_executor()->run(
        jacobi::make_find_blocks(first_system, parameters_.max_block_size,
                                 num_blocks_, parameters_.block_pointers));
}


template <typename ValueType, typename IndexType>
void Jacobi<ValueType, IndexType>::generate_precond(
    const BatchLinOp* const system_matrix)
{
    using unbatch_type = gko::matrix::Csr<ValueType, IndexType>;
    // generate entire batch of factorizations
    auto exec = this->get_executor();

    if (parameters_.max_block_size == 1u) {
        // External generate does nothing in case of scalar block jacobi (as the
        // whole generation is done inside the solver kernel)
        num_blocks_ = system_matrix->get_common_size()[0];
        blocks_ = gko::array<ValueType>(exec);
        parameters_.block_pointers = gko::array<IndexType>(exec);
        return;
    }

    std::shared_ptr<matrix_type> sys_csr;

    if (auto temp_csr = dynamic_cast<const matrix_type*>(system_matrix)) {
        sys_csr = gko::share(gko::clone(exec, temp_csr));
    } else {
        sys_csr = gko::share(matrix_type::create(exec));
        as<ConvertibleTo<matrix_type>>(system_matrix)
            ->convert_to(sys_csr.get());
    }

    const auto num_batch = sys_csr->get_num_batch_items();
    const auto num_rows = sys_csr->get_common_size()[0];
    const auto num_nz = sys_csr->get_num_elements_per_item();

    // extract the first matrix, as a view, into a regular Csr matrix.
    const auto unbatch_size =
        gko::dim<2>{num_rows, sys_csr->get_common_size()[1]};
    auto sys_rows_view = array<IndexType>::const_view(
        exec, num_rows + 1, sys_csr->get_const_row_ptrs());
    auto sys_cols_view = array<IndexType>::const_view(
        exec, num_nz, sys_csr->get_const_col_idxs());
    auto sys_vals_view =
        array<ValueType>::const_view(exec, num_nz, sys_csr->get_const_values());
    auto first_sys_csr = gko::share(unbatch_type::create_const(
        exec, unbatch_size, std::move(sys_vals_view), std::move(sys_cols_view),
        std::move(sys_rows_view)));

    if (parameters_.block_pointers.get_data() == nullptr) {
        this->detect_blocks(num_batch, first_sys_csr.get());
        exec->synchronize();
        blocks_cumulative_storage_.resize_and_reset(num_blocks_ + 1);
    }

    // cumulative block storage
    exec->run(jacobi::make_compute_cumulative_block_storage(
        num_blocks_, parameters_.block_pointers.get_const_data(),
        blocks_cumulative_storage_.get_data()));

    blocks_.resize_and_reset(this->compute_storage_space(num_batch));

    exec->run(jacobi::make_find_row_is_part_of_which_block(
        num_blocks_, parameters_.block_pointers.get_const_data(),
        row_part_of_which_block_info_.get_data()));

    // Note: Row-major order offers advantage in terms of
    // performance in both preconditioner generation and application for both
    // reference and cuda backend.
    // Note: The pattern blocks in block_pattern are
    // also stored in a similar way.

    // array for storing the common pattern of the diagonal blocks
    gko::array<IndexType> blocks_pattern(exec, this->compute_storage_space(1));
    blocks_pattern.fill(static_cast<IndexType>(-1));

    // Since all the matrices in the batch have the same sparisty pattern, it is
    // advantageous to extract the blocks only once instead of repeating
    // computations for each matrix entry. Thus, first, a common pattern for the
    // blocks (corresponding to a batch entry) is extracted and then blocks
    // corresponding to different batch entries are obtained by just filling in
    // values based on the common pattern.
    exec->run(jacobi::make_extract_common_blocks_pattern(
        first_sys_csr.get(), num_blocks_, blocks_storage_scheme_,
        blocks_cumulative_storage_.get_const_data(),
        parameters_.block_pointers.get_const_data(),
        row_part_of_which_block_info_.get_const_data(),
        blocks_pattern.get_data()));

    exec->run(jacobi::make_compute_block_jacobi(
        sys_csr.get(), parameters_.max_block_size, num_blocks_,
        blocks_storage_scheme_, blocks_cumulative_storage_.get_const_data(),
        parameters_.block_pointers.get_const_data(),
        blocks_pattern.get_const_data(), blocks_.get_data()));
}


#define GKO_DECLARE_BATCH_JACOBI(_type) class Jacobi<_type, int32>
GKO_INSTANTIATE_FOR_EACH_VALUE_TYPE(GKO_DECLARE_BATCH_JACOBI);


}  // namespace preconditioner
}  // namespace batch
}  // namespace gko
