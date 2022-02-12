/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2022, the Ginkgo authors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************<GINKGO LICENSE>*******************************/

#include "core/multigrid/amgx_pgm_kernels.hpp"


#include <ginkgo/core/base/math.hpp>


#include "common/unified/base/kernel_launch.hpp"
#include "common/unified/base/kernel_launch_reduction.hpp"
#include "core/components/prefix_sum_kernels.hpp"


namespace gko {
namespace kernels {
namespace GKO_DEVICE_NAMESPACE {
/**
 * @brief The AmgxPgm namespace.
 *
 * @ingroup amgx_pgm
 */
namespace amgx_pgm {


template <typename IndexType>
void match_edge(std::shared_ptr<const DefaultExecutor> exec,
                const Array<IndexType>& strongest_neighbor,
                Array<IndexType>& agg)
{
    run_kernel(
        exec,
        [] GKO_KERNEL(auto tidx, auto strongest_neighbor_vals, auto agg_vals) {
            if (agg_vals[tidx] != -1) {
                return;
            }
            auto neighbor = strongest_neighbor_vals[tidx];
            if (neighbor != -1 && strongest_neighbor_vals[neighbor] == tidx &&
                tidx <= neighbor) {
                // Use the smaller index as agg point
                agg_vals[tidx] = tidx;
                agg_vals[neighbor] = tidx;
            }
        },
        agg.get_num_elems(), strongest_neighbor.get_const_data(),
        agg.get_data());
}

GKO_INSTANTIATE_FOR_EACH_INDEX_TYPE(GKO_DECLARE_AMGX_PGM_MATCH_EDGE_KERNEL);


template <typename IndexType>
void count_unagg(std::shared_ptr<const DefaultExecutor> exec,
                 const Array<IndexType>& agg, IndexType* num_unagg)
{
    Array<IndexType> d_result(exec, 1);
    run_kernel_reduction(
        exec, [] GKO_KERNEL(auto i, auto array) { return array[i] == -1; },
        GKO_KERNEL_REDUCE_SUM(IndexType), d_result.get_data(),
        agg.get_num_elems(), agg);

    *num_unagg = exec->copy_val_to_host(d_result.get_const_data());
}

GKO_INSTANTIATE_FOR_EACH_INDEX_TYPE(GKO_DECLARE_AMGX_PGM_COUNT_UNAGG_KERNEL);


template <typename IndexType>
void renumber(std::shared_ptr<const DefaultExecutor> exec,
              Array<IndexType>& agg, IndexType* num_agg)
{
    const auto num = agg.get_num_elems();
    Array<IndexType> agg_map(exec, num + 1);
    run_kernel(
        exec,
        [] GKO_KERNEL(auto tidx, auto agg, auto agg_map) {
            // agg_vals[i] == i always holds in the aggregated group whose
            // identifier is
            // i because we use the index of element as the aggregated group
            // identifier.
            agg_map[tidx] = (agg[tidx] == tidx);
        },
        num, agg.get_const_data(), agg_map.get_data());

    components::prefix_sum(exec, agg_map.get_data(), agg_map.get_num_elems());

    run_kernel(
        exec,
        [] GKO_KERNEL(auto tidx, auto map, auto agg) {
            agg[tidx] = map[agg[tidx]];
        },
        num, agg_map.get_const_data(), agg.get_data());
    *num_agg = exec->copy_val_to_host(agg_map.get_const_data() + num);
}

GKO_INSTANTIATE_FOR_EACH_INDEX_TYPE(GKO_DECLARE_AMGX_PGM_RENUMBER_KERNEL);


template <typename ValueType, typename IndexType>
void find_strongest_neighbor(
    std::shared_ptr<const DefaultExecutor> exec,
    const matrix::Csr<ValueType, IndexType>* weight_mtx,
    const matrix::Diagonal<ValueType>* diag, Array<IndexType>& agg,
    Array<IndexType>& strongest_neighbor)
{
    run_kernel(
        exec,
        [] GKO_KERNEL(auto row, auto row_ptrs, auto col_idxs, auto weight_vals,
                      auto diag, auto agg, auto strongest_neighbor) {
            auto max_weight_unagg = zero<ValueType>();
            auto max_weight_agg = zero<ValueType>();
            IndexType strongest_unagg = -1;
            IndexType strongest_agg = -1;
            if (agg[row] != -1) {
                return;
            }
            for (auto idx = row_ptrs[row]; idx < row_ptrs[row + 1]; idx++) {
                auto col = col_idxs[idx];
                if (col == row) {
                    continue;
                }
                auto weight =
                    weight_vals[idx] / max(abs(diag[row]), abs(diag[col]));
                if (agg[col] == -1 &&
                    device_std::tie(weight, col) >
                        device_std::tie(max_weight_unagg, strongest_unagg)) {
                    max_weight_unagg = weight;
                    strongest_unagg = col;
                } else if (agg[col] != -1 &&
                           device_std::tie(weight, col) >
                               device_std::tie(max_weight_agg, strongest_agg)) {
                    max_weight_agg = weight;
                    strongest_agg = col;
                }
            }

            if (strongest_unagg == -1 && strongest_agg != -1) {
                // all neighbor is agg, connect to the strongest agg
                // Also, no others will use this item as their
                // strongest_neighbor because they are already aggregated. Thus,
                // it is determinstic behavior
                agg[row] = agg[strongest_agg];
            } else if (strongest_unagg != -1) {
                // set the strongest neighbor in the unagg group
                strongest_neighbor[row] = strongest_unagg;
            } else {
                // no neighbor
                strongest_neighbor[row] = row;
            }
        },
        agg.get_num_elems(), weight_mtx->get_const_row_ptrs(),
        weight_mtx->get_const_col_idxs(), weight_mtx->get_const_values(),
        diag->get_const_values(), agg.get_data(),
        strongest_neighbor.get_data());
}

GKO_INSTANTIATE_FOR_EACH_NON_COMPLEX_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_AMGX_PGM_FIND_STRONGEST_NEIGHBOR);

template <typename ValueType, typename IndexType>
void assign_to_exist_agg(std::shared_ptr<const DefaultExecutor> exec,
                         const matrix::Csr<ValueType, IndexType>* weight_mtx,
                         const matrix::Diagonal<ValueType>* diag,
                         Array<IndexType>& agg,
                         Array<IndexType>& intermediate_agg)
{
    const auto num = agg.get_num_elems();
    if (intermediate_agg.get_num_elems() > 0) {
        // determinstic kernel
        run_kernel(
            exec,
            [] GKO_KERNEL(auto row, auto row_ptrs, auto col_idxs,
                          auto weight_vals, auto diag, auto agg_const_val,
                          auto agg_val) {
                if (agg_val[row] != -1) {
                    return;
                }
                ValueType max_weight_agg = zero<ValueType>();
                IndexType strongest_agg = -1;
                for (auto idx = row_ptrs[row]; idx < row_ptrs[row + 1]; idx++) {
                    auto col = col_idxs[idx];
                    if (col == row) {
                        continue;
                    }
                    auto weight =
                        weight_vals[idx] / max(abs(diag[row]), abs(diag[col]));
                    if (agg_const_val[col] != -1 &&
                        device_std::tie(weight, col) >
                            device_std::tie(max_weight_agg, strongest_agg)) {
                        max_weight_agg = weight;
                        strongest_agg = col;
                    }
                }
                if (strongest_agg != -1) {
                    agg_val[row] = agg_const_val[strongest_agg];
                } else {
                    agg_val[row] = row;
                }
            },
            num, weight_mtx->get_const_row_ptrs(),
            weight_mtx->get_const_col_idxs(), weight_mtx->get_const_values(),
            diag->get_const_values(), agg.get_const_data(),
            intermediate_agg.get_data());
        // Copy the intermediate_agg to agg
        agg = intermediate_agg;
    } else {
        // undeterminstic kernel
        run_kernel(
            exec,
            [] GKO_KERNEL(auto row, auto row_ptrs, auto col_idxs,
                          auto weight_vals, auto diag, auto agg_val) {
                if (agg_val[row] != -1) {
                    return;
                }
                ValueType max_weight_agg = zero<ValueType>();
                IndexType strongest_agg = -1;
                for (auto idx = row_ptrs[row]; idx < row_ptrs[row + 1]; idx++) {
                    auto col = col_idxs[idx];
                    if (col == row) {
                        continue;
                    }
                    auto weight =
                        weight_vals[idx] / max(abs(diag[row]), abs(diag[col]));
                    if (agg_val[col] != -1 &&
                        device_std::tie(weight, col) >
                            device_std::tie(max_weight_agg, strongest_agg)) {
                        max_weight_agg = weight;
                        strongest_agg = col;
                    }
                }
                if (strongest_agg != -1) {
                    agg_val[row] = agg_val[strongest_agg];
                } else {
                    agg_val[row] = row;
                }
            },
            num, weight_mtx->get_const_row_ptrs(),
            weight_mtx->get_const_col_idxs(), weight_mtx->get_const_values(),
            diag->get_const_values(), agg.get_data());
    }
}

GKO_INSTANTIATE_FOR_EACH_NON_COMPLEX_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_AMGX_PGM_ASSIGN_TO_EXIST_AGG);


}  // namespace amgx_pgm
}  // namespace GKO_DEVICE_NAMESPACE
}  // namespace kernels
}  // namespace gko