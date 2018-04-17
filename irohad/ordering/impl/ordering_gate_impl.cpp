/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utility>

#include "ordering/impl/ordering_gate_impl.hpp"

#include "interfaces/iroha_internal/block.hpp"
#include "interfaces/iroha_internal/proposal.hpp"
#include "interfaces/transaction.hpp"

namespace iroha {
  namespace ordering {

    bool ProposalComparator::operator()(
        const std::shared_ptr<shared_model::interface::Proposal> &lhs,
        const std::shared_ptr<shared_model::interface::Proposal> &rhs) const {
      return lhs->height() > rhs->height();
    }

    OrderingGateImpl::OrderingGateImpl(
        std::shared_ptr<iroha::network::OrderingGateTransport> transport)
        : transport_(std::move(transport)),
          last_block_height_(1),
          log_(logger::log("OrderingGate")) {}

    void OrderingGateImpl::propagateTransaction(
        std::shared_ptr<const shared_model::interface::Transaction>
            transaction) {
      log_->info("propagate tx, account_id: {}",
                 " account_id: " + transaction->creatorAccountId());

      transport_->propagateTransaction(transaction);
    }

    rxcpp::observable<std::shared_ptr<shared_model::interface::Proposal>>
    OrderingGateImpl::on_proposal() {
      return proposals_.get_observable();
    }

    void OrderingGateImpl::setPcs(
        const iroha::network::PeerCommunicationService &pcs) {
      pcs_subscriber_ = pcs.on_commit().subscribe([this](const auto &block) {
        unlock_next_.store(true);
        // find height of last commited block
        block.subscribe([this](const auto &b) {
          if (b->height() > this->last_block_height_) {
            this->last_block_height_ = b->height();
          }
        });
        this->tryNextRound();
      });
    }

    void OrderingGateImpl::onProposal(
        std::shared_ptr<shared_model::interface::Proposal> proposal) {
      log_->info("Received new proposal, height: {}", proposal->height());
      proposal_queue_.push(std::move(proposal));
      tryNextRound();
    }

    void OrderingGateImpl::tryNextRound() {
      while (not proposal_queue_.empty() and unlock_next_) {
        std::shared_ptr<shared_model::interface::Proposal> next_proposal;
        proposal_queue_.try_pop(next_proposal);
        // check for old proposal
        if (next_proposal->height() < last_block_height_ + 1) {
          log_->info("Old proposal, discarding");
          continue;
        }
        // check for new proposal
        if (next_proposal->height() > last_block_height_ + 1) {
          log_->info("Proposal newer than last block, keeping in queue");
          proposal_queue_.push(next_proposal);
          unlock_next_.store(false);
          continue;
        }
        log_->info("Pass the proposal to pipeline");
        unlock_next_.store(false);
        proposals_.get_subscriber().on_next(next_proposal);
      }
    }

    OrderingGateImpl::~OrderingGateImpl() {
      pcs_subscriber_.unsubscribe();
    }

  }  // namespace ordering
}  // namespace iroha
