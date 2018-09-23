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
#include <bitcoin/node/configuration.hpp>

#include <cstddef>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/network.hpp>

namespace libbitcoin {
namespace node {

// Construct with defaults and no context
configuration::configuration()
{
    version = false;
    settings = false;
    help = false;
    initchain = false;
    regtest = false;
    testnet = false;
    bitcoin = NULL;
    chain = NULL;
    node = NULL;
    database = NULL;
    network = NULL;
}

configuration::~configuration()
{
    if (node)
        delete node;
    if (chain)
        delete chain;
    if (database)
        delete database;
    if (network)
        delete network;
    if (bitcoin)
        delete bitcoin;
    }

// Initialize for the given context
void configuration::init(config::settings context)
{
    node = new node::settings(context);
    chain = new blockchain::settings(context);
    database = new database::settings(context);
    network = new network::settings(context);
    bitcoin = new libbitcoin::settings(context);
    
    using serve = message::version::service;

    if (database)
    {
        // A node doesn't use history, and history is expensive.
        database->index_addresses = false;
    }

    if (network)
    {
        // Logs will slow things if not rotated.
        network->rotation_size = 10000000;

        // It is a public network.
        network->inbound_connections = 100;
        
        // Optimal for sync and network penetration.
        network->outbound_connections = 8;
        
        // A node allows 10000 host names by default.
        network->host_pool_capacity = 10000;
        
        // Expose full node (1) and witness (8) network services by default.
        network->services = serve::node_network | serve::node_witness;
    }
}

} // namespace node
} // namespace libbitcoin
