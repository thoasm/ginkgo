/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2023, the Ginkgo authors
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

#include "core/solver/batch_bicgstab_kernels.hpp"


#include <memory>
#include <random>


#include <gtest/gtest.h>


#include <ginkgo/core/base/batch_multi_vector.hpp>
#include <ginkgo/core/log/batch_logger.hpp>
#include <ginkgo/core/matrix/batch_dense.hpp>
#include <ginkgo/core/matrix/batch_ell.hpp>
#include <ginkgo/core/solver/batch_bicgstab.hpp>


#include "core/base/batch_utilities.hpp"
#include "core/matrix/batch_dense_kernels.hpp"
#include "core/test/utils.hpp"
#include "core/test/utils/batch_helpers.hpp"
#include "test/utils/executor.hpp"


class BatchBicgstab : public CommonTestFixture {
protected:
    using real_type = gko::remove_complex<value_type>;
    using solver_type = gko::batch::solver::Bicgstab<value_type>;
    using Mtx = gko::batch::matrix::Dense<value_type>;
    using EllMtx = gko::batch::matrix::Ell<value_type>;
    using MVec = gko::batch::MultiVector<value_type>;
    using RealMVec = gko::batch::MultiVector<real_type>;
    using Settings = gko::kernels::batch_bicgstab::BicgstabSettings<real_type>;
    using LogData = gko::batch::log::BatchLogData<real_type>;
    using Logger = gko::batch::log::BatchConvergence<real_type>;

    BatchBicgstab() {}

    template <typename MatrixType, typename... MatrixArgs>
    gko::test::LinearSystem<MatrixType> setup_linsys_and_solver(
        const gko::size_type num_batch_items, const int num_rows,
        const int num_rhs, const real_type tol, const int max_iters,
        MatrixArgs&&... args)
    {
        auto executor = exec;
        solve_lambda = [executor](const Settings settings,
                                  const gko::batch::BatchLinOp* prec,
                                  const Mtx* mtx, const MVec* b, MVec* x,
                                  LogData& log_data) {
            gko::kernels::EXEC_NAMESPACE::batch_bicgstab::apply<
                typename Mtx::value_type>(executor, settings, mtx, prec, b, x,
                                          log_data);
        };
        solver_settings =
            Settings{max_iters, tol, gko::batch::stop::ToleranceType::relative};

        solver_factory =
            solver_type::build()
                .with_default_max_iterations(max_iters)
                .with_default_tolerance(tol)
                .with_tolerance_type(gko::batch::stop::ToleranceType::relative)
                .on(exec);
        return gko::test::generate_3pt_stencil_batch_problem<MatrixType>(
            exec, num_batch_items, num_rows, num_rhs,
            std::forward<MatrixArgs>(args)...);
    }

    std::function<void(const Settings, const gko::batch::BatchLinOp*,
                       const Mtx*, const MVec*, MVec*, LogData&)>
        solve_lambda;
    Settings solver_settings{};
    std::shared_ptr<solver_type::Factory> solver_factory;
};


TEST_F(BatchBicgstab, SolvesStencilSystem)
{
    const int num_batch_items = 2;
    const int num_rows = 10;
    const int num_rhs = 1;
    const real_type tol = 1e-5;
    const int max_iters = 100;
    auto linear_system = setup_linsys_and_solver<Mtx>(num_batch_items, num_rows,
                                                      num_rhs, tol, max_iters);

    auto res = gko::test::solve_linear_system(exec, solve_lambda,
                                              solver_settings, linear_system);

    for (size_t i = 0; i < num_batch_items; i++) {
        ASSERT_LE(res.res_norm->get_const_values()[i] /
                      linear_system.rhs_norm->get_const_values()[i],
                  solver_settings.residual_tol);
    }
    GKO_ASSERT_BATCH_MTX_NEAR(res.x, linear_system.exact_sol, tol);
}


TEST_F(BatchBicgstab, StencilSystemLoggerLogsResidual)
{
    const int num_batch_items = 2;
    const int num_rows = 10;
    const int num_rhs = 1;
    const real_type tol = 1e-5;
    const int max_iters = 100;
    auto linear_system = setup_linsys_and_solver<Mtx>(num_batch_items, num_rows,
                                                      num_rhs, tol, max_iters);

    auto res = gko::test::solve_linear_system(exec, solve_lambda,
                                              solver_settings, linear_system);

    auto res_log_array = res.log_data->res_norms.get_const_data();
    for (size_t i = 0; i < num_batch_items; i++) {
        ASSERT_LE(res_log_array[i] / linear_system.rhs_norm->at(i, 0, 0),
                  solver_settings.residual_tol);
        ASSERT_NEAR(res_log_array[i], res.res_norm->get_const_values()[i],
                    10 * tol);
    }
}


TEST_F(BatchBicgstab, StencilSystemLoggerLogsIterations)
{
    const int num_batch_items = 2;
    const int num_rows = 10;
    const int num_rhs = 1;
    const int ref_iters = 5;
    auto linear_system = setup_linsys_and_solver<Mtx>(num_batch_items, num_rows,
                                                      num_rhs, 0, ref_iters);

    auto res = gko::test::solve_linear_system(exec, solve_lambda,
                                              solver_settings, linear_system);

    auto iter_array = res.log_data->iter_counts.get_const_data();
    for (size_t i = 0; i < num_batch_items; i++) {
        ASSERT_EQ(iter_array[i], ref_iters);
    }
}


TEST_F(BatchBicgstab, CanSolve3ptStencilSystem)
{
    const int num_batch_items = 12;
    const int num_rows = 100;
    const int num_rhs = 1;
    const real_type tol = 1e-5;
    const int max_iters = 100;
    auto linear_system = setup_linsys_and_solver<Mtx>(num_batch_items, num_rows,
                                                      num_rhs, tol, max_iters);
    auto solver = gko::share(solver_factory->generate(linear_system.matrix));

    auto res = gko::test::solve_linear_system(exec, linear_system, solver);

    GKO_ASSERT_BATCH_MTX_NEAR(res.x, linear_system.exact_sol, tol * 10);
    for (size_t i = 0; i < num_batch_items; i++) {
        auto comp_res_norm =
            exec->copy_val_to_host(res.res_norm->get_const_values() + i) /
            exec->copy_val_to_host(linear_system.rhs_norm->get_const_values() +
                                   i);
        ASSERT_LE(comp_res_norm, tol);
    }
}


TEST_F(BatchBicgstab, CanSolveLargeHpdSystem)
{
    const int num_batch_items = 3;
    const int num_rows = 1025;
    const int num_rhs = 1;
    const real_type tol = 1e-5;
    const int max_iters = 2000;
    const real_type comp_tol = tol * 100;
    auto solver_factory =
        solver_type::build()
            .with_default_max_iterations(max_iters)
            .with_default_tolerance(tol)
            .with_tolerance_type(gko::batch::stop::ToleranceType::absolute)
            .on(exec);
    std::shared_ptr<Logger> logger = Logger::create(exec);
    auto linear_system = gko::test::generate_diag_dominant_batch_problem<Mtx>(
        exec, num_batch_items, num_rows, num_rhs, true);
    auto solver = gko::share(solver_factory->generate(linear_system.matrix));
    solver->add_logger(logger);

    auto res = gko::test::solve_linear_system(exec, linear_system, solver);

    solver->remove_logger(logger);
    auto iter_counts = gko::make_temporary_clone(exec->get_master(),
                                                 &logger->get_num_iterations());
    auto res_norm = gko::make_temporary_clone(exec->get_master(),
                                              &logger->get_residual_norm());
    GKO_ASSERT_BATCH_MTX_NEAR(res.x, linear_system.exact_sol, comp_tol);
    for (size_t i = 0; i < num_batch_items; i++) {
        auto comp_res_norm =
            exec->copy_val_to_host(res.res_norm->get_const_values() + i);
        ASSERT_LE(iter_counts->get_const_data()[i], max_iters);
        EXPECT_LE(res_norm->get_const_data()[i], comp_tol);
        EXPECT_GT(res_norm->get_const_data()[i], real_type{0.0});
        ASSERT_LE(comp_res_norm, comp_tol);
    }
}
