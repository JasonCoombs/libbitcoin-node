/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/full_node.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <boost/range/adaptor/reversed.hpp>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/node/configuration.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/sessions/session_inbound.hpp>
#include <bitcoin/node/sessions/session_manual.hpp>
#include <bitcoin/node/sessions/session_outbound.hpp>

namespace libbitcoin {
namespace node {

using namespace bc::blockchain;
using namespace bc::chain;
using namespace bc::config;
using namespace bc::network;
using namespace boost::adaptors;
using namespace std::placeholders;

full_node::full_node(const configuration& configuration)
  : p2p(configuration.network),
    reservations_(configuration.network.minimum_connections(),
        configuration.node.maximum_deviation,
        configuration.node.block_latency_seconds),
    chain_(thread_pool(), configuration.chain, configuration.database,
        configuration.bitcoin),
    protocol_maximum_(configuration.network.protocol_maximum),
    chain_settings_(configuration.chain),
    node_settings_(configuration.node)
{
}

full_node::~full_node()
{
    full_node::close();
}

// Start.
// ----------------------------------------------------------------------------

void full_node::start(result_handler handler)
{
    if (!stopped())
    {
        handler(error::operation_failed);
        return;
    }

    if (!chain_.start())
    {
        LOG_ERROR(LOG_NODE)
            << "Failure starting blockchain.";
        handler(error::operation_failed);
        return;
    }

    // This is invoked on the same thread.
    // Stopped is true and no network threads until after this call.
    p2p::start(handler);
}

// Run sequence.
// ----------------------------------------------------------------------------

// This follows seeding as an explicit step, sync hooks may go here.
void full_node::run(result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    handle_running(error::success, handler);
}

void full_node::handle_running(const code& ec, result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        LOG_ERROR(LOG_NODE)
            << "Failure synchronizing blocks: " << ec.message();
        handler(ec);
        return;
    }

    checkpoint confirmed;
    if (!chain_.get_top(confirmed, false))
    {
        LOG_ERROR(LOG_NODE)
            << "The block chain is corrupt.";
        handler(error::operation_failed);
        return;
    }

    set_top_block(confirmed);
    LOG_INFO(LOG_NODE)
        << "Top confirmed block height is (" << confirmed.height() << ").";

    checkpoint candidate;
    if (!chain_.get_top(candidate, true))
    {
        LOG_ERROR(LOG_NODE)
            << "The candidate chain is corrupt.";
        handler(error::operation_failed);
        return;
    }

    set_top_header(candidate);
    LOG_INFO(LOG_NODE)
        << "Top candidate block height is (" << candidate.height() << ").";

    hash_digest hash;
    auto top_valid = chain_.top_valid_candidate_state()->height();
    const auto start_height = top_valid + 1u;

    LOG_INFO(LOG_NODE)
        << "Top valid candidate block height (" << top_valid << ").";

    // Scan header index from top down until just after last valid block.
    // This may re-download non-empty blocks, which prevents stall in the
    // case where next candidate after last valid (start_height) is non-empty.
    // Genesis ensures loop termination, and its existence is guaranteed above.
    for (auto height = candidate.height(); height > top_valid; --height)
        if (chain_.get_downloadable(hash, height) || height == start_height)
            reservations_.push_front(std::move(hash), height);

    LOG_INFO(LOG_NODE)
        << "Pending block downloads (" << reservations_.size() << ").";

    subscribe_headers(
        std::bind(&full_node::handle_reindexed,
            this, _1, _2, _3, _4));

    subscribe_blocks(
        std::bind(&full_node::handle_reorganized,
            this, _1, _2, _3, _4));

    // This is invoked on a new thread.
    // This is the end of the derived run startup sequence.
    p2p::run(handler);
}

// A typical reorganization consists of one incoming and zero outgoing blocks.
bool full_node::handle_reindexed(code ec, size_t fork_height,
    header_const_ptr_list_const_ptr incoming,
    header_const_ptr_list_const_ptr outgoing)
{
    if (stopped() || ec == error::service_stopped)
        return false;

    if (ec)
    {
        LOG_ERROR(LOG_NODE)
            << "Failure handling reindex: " << ec.message();
        stop();
        return false;
    }

    // Nothing to do here.
    if (!incoming || incoming->empty())
        return true;

    // First pop height is highest outgoing.
    auto height = fork_height + outgoing->size();

    // Pop outgoing reservations from download queue (if at top), high first.
    for (const auto header: reverse(*outgoing))
        reservations_.pop_back(*header, height--);

    // Push unpopulated incoming reservations (can't expect parent), low first.
    for (const auto header: *incoming)
        reservations_.push_back(*header, ++height);

    // Top height will be: fork_height + incoming->size();
    set_top_header({ incoming->back()->hash(), height });
    return true;
}

// A typical reorganization consists of one incoming and zero outgoing blocks.
bool full_node::handle_reorganized(code ec, size_t fork_height,
    block_const_ptr_list_const_ptr incoming,
    block_const_ptr_list_const_ptr outgoing)
{
    if (stopped() || ec == error::service_stopped)
        return false;

    if (ec)
    {
        LOG_ERROR(LOG_NODE)
            << "Failure handling reorganization: " << ec.message();
        stop();
        return false;
    }

    // Nothing to do here.
    if (!incoming || incoming->empty())
        return true;

    for (const auto block: *outgoing)
    {
        LOG_DEBUG(LOG_NODE)
            << "Reorganization moved block to pool ["
            << encode_hash(block->header().hash()) << "]";
    }

    const auto height = fork_height + incoming->size();
    set_top_block({ incoming->back()->hash(), height });
    return true;
}

// Specializations.
// ----------------------------------------------------------------------------
// Create derived sessions and override these to inject from derived node.

// Must not connect until running, otherwise messages may conflict with sync.
// But we establish the session in network so caller doesn't need to run.
network::session_manual::ptr full_node::attach_manual_session()
{
    return attach<node::session_manual>(chain_);
}

network::session_inbound::ptr full_node::attach_inbound_session()
{
    return attach<node::session_inbound>(chain_);
}

network::session_outbound::ptr full_node::attach_outbound_session()
{
    return attach<node::session_outbound>(chain_);
}

// Shutdown
// ----------------------------------------------------------------------------

bool full_node::stop()
{
    // Suspend new work last so we can use work to clear subscribers.
    const auto p2p_stop = p2p::stop();
    const auto chain_stop = chain_.stop();

    if (!p2p_stop)
        LOG_ERROR(LOG_NODE)
            << "Failed to stop network.";

    if (!chain_stop)
        LOG_ERROR(LOG_NODE)
            << "Failed to stop blockchain.";

    return p2p_stop && chain_stop;
}

// This must be called from the thread that constructed this class (see join).
bool full_node::close()
{
    // Invoke own stop to signal work suspension.
    if (!full_node::stop())
        return false;

    const auto p2p_close = p2p::close();
    const auto chain_close = chain_.close();

    if (!p2p_close)
        LOG_ERROR(LOG_NODE)
            << "Failed to close network.";

    if (!chain_close)
        LOG_ERROR(LOG_NODE)
            << "Failed to close blockchain.";

    return p2p_close && chain_close;
}

// Properties.
// ----------------------------------------------------------------------------

const node::settings& full_node::node_settings() const
{
    return node_settings_;
}

const blockchain::settings& full_node::chain_settings() const
{
    return chain_settings_;
}

safe_chain& full_node::chain()
{
    return chain_;
}

reservation::ptr full_node::get_reservation()
{
    return reservations_.get();
}

// Subscriptions.
// ----------------------------------------------------------------------------

void full_node::subscribe_blocks(block_handler&& handler)
{
    chain().subscribe_blocks(std::move(handler));
}

void full_node::subscribe_headers(header_handler&& handler)
{
    chain().subscribe_headers(std::move(handler));
}

void full_node::subscribe_transactions(transaction_handler&& handler)
{
    chain().subscribe_transactions(std::move(handler));
}

} // namespace node
} // namespace libbitcoin
