/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2021, the Ginkgo authors
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

#ifndef GKO_PUBLIC_CORE_LOG_CONVERGENCE_HPP_
#define GKO_PUBLIC_CORE_LOG_CONVERGENCE_HPP_


#include <memory>


#include <ginkgo/core/log/logger.hpp>
#include <ginkgo/core/matrix/dense.hpp>
#include <ginkgo/core/stop/criterion.hpp>


namespace gko {
/**
 * @brief The logger namespace .
 * @ref log
 * @ingroup log
 */
namespace log {


/**
 * Convergence is a Logger which logs data strictly from the
 * `criterion_check_completed` event. The purpose of this logger is to give a
 * simple access to standard data generated by the solver once it has stopped
 * with minimal overhead.
 *
 * This logger also computes the residual norm from the residual when the
 * residual norm was not available. This can add some slight overhead.
 *
 * @ingroup log
 */
template <typename ValueType = default_precision>
class Convergence : public Logger {
public:
    void on_criterion_check_completed(
        const stop::Criterion *criterion, const size_type &num_iterations,
        const LinOp *residual, const LinOp *residual_norm,
        const LinOp *solution, const uint8 &stopping_id,
        const bool &set_finalized, const Array<stopping_status> *status,
        const bool &one_changed, const bool &all_stopped) const override;

    void on_criterion_check_completed(
        const stop::Criterion *criterion, const size_type &num_iterations,
        const LinOp *residual, const LinOp *residual_norm,
        const LinOp *implicit_sq_resnorm, const LinOp *solution,
        const uint8 &stopping_id, const bool &set_finalized,
        const Array<stopping_status> *status, const bool &one_changed,
        const bool &all_stopped) const override;

    /**
     * Creates a convergence logger. This dynamically allocates the memory,
     * constructs the object and returns an std::unique_ptr to this object.
     *
     * @param exec  the executor
     * @param enabled_events  the events enabled for this logger. By default all
     *                        events.
     *
     * @return an std::unique_ptr to the the constructed object
     *
     * @internal here I cannot use EnableCreateMethod due to complex circular
     * dependencies. At the same time, this method is short enough that it
     * shouldn't be a problem.
     */
    static std::unique_ptr<Convergence> create(
        std::shared_ptr<const Executor> exec,
        const mask_type &enabled_events = Logger::all_events_mask)
    {
        return std::unique_ptr<Convergence>(
            new Convergence(exec, enabled_events));
    }

    /**
     * Returns true if the solver has converged.
     *
     * @return the bool flag for convergence status
     */
    bool has_converged() const noexcept { return convergence_status_; }

    /**
     * Resets the convergence status to false.
     */
    void reset_convergence_status() { this->convergence_status_ = false; }

    /**
     * Returns the number of iterations
     *
     * @return the number of iterations
     */
    const size_type &get_num_iterations() const noexcept
    {
        return num_iterations_;
    }

    /**
     * Returns the residual
     *
     * @return the residual
     */
    const LinOp *get_residual() const noexcept { return residual_.get(); }

    /**
     * Returns the residual norm
     *
     * @return the residual norm
     */
    const LinOp *get_residual_norm() const noexcept
    {
        return residual_norm_.get();
    }

    /**
     * Returns the implicit squared residual norm
     *
     * @return the implicit squared residual norm
     */
    const LinOp *get_implicit_sq_resnorm() const noexcept
    {
        return implicit_sq_resnorm_.get();
    }

protected:
    /**
     * Creates a Convergence logger.
     *
     * @param exec  the executor
     * @param enabled_events  the events enabled for this logger. By default all
     *                        events.
     */
    explicit Convergence(
        std::shared_ptr<const gko::Executor> exec,
        const mask_type &enabled_events = Logger::all_events_mask)
        : Logger(exec, enabled_events)
    {}

private:
    mutable bool convergence_status_{false};
    mutable size_type num_iterations_{};
    mutable std::unique_ptr<LinOp> residual_{};
    mutable std::unique_ptr<LinOp> residual_norm_{};
    mutable std::unique_ptr<LinOp> implicit_sq_resnorm_{};
};


}  // namespace log
}  // namespace gko


#endif  // GKO_PUBLIC_CORE_LOG_CONVERGENCE_HPP_
