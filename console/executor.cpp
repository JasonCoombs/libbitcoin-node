/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-node.
 *
 * libbitcoin-node is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "executor.hpp"

#include <chrono>
#include <csignal>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <boost/format.hpp>
#include <bitcoin/node.hpp>

namespace libbitcoin {
namespace node {

using boost::format;
using std::placeholders::_1;
using namespace boost::system;
using namespace bc::blockchain;
using namespace bc::config;
using namespace bc::database;
using namespace bc::network;

static constexpr int no_interrupt = 0;
static constexpr int directory_exists = 0;
static constexpr int directory_not_found = 2;
static constexpr auto append = std::ofstream::out | std::ofstream::app;

static const auto application_name = "bn";
static const auto stop_sensitivity = std::chrono::milliseconds(10);

// Static interrupt state (unavoidable).
static auto stopped_ = false;

// Static handler for catching termination signals.
static void initialize_interrupt(int code)
{
    // Reinitialize after each capture.
    signal(SIGINT, initialize_interrupt);
    signal(SIGTERM, initialize_interrupt);
    signal(SIGABRT, initialize_interrupt);

    // The no_interrupt sentinel is used for first initialization.
    if (code == no_interrupt)
    {
        log::info(LOG_NODE) << BN_NODE_INTERRUPT;
        return;
    }

    // Signal the service to stop if not already signaled.
    if (!stopped_)
    {
        log::info(LOG_NODE) << format(BN_NODE_STOPPING) % code;
        stopped_ = true;
    }
}

executor::executor(parser& metadata, std::istream& input,
    std::ostream& output, std::ostream& error)
  : metadata_(metadata), output_(output),
    debug_file_(metadata_.configured.network.debug_file.string(), append),
    error_file_(metadata_.configured.network.error_file.string(), append)
{
    initialize_logging(debug_file_, error_file_, output, error);
}

void executor::initialize_output()
{
    log::debug(LOG_NODE) << BN_LOG_HEADER;
    log::info(LOG_NODE) << BN_LOG_HEADER;
    log::warning(LOG_NODE) << BN_LOG_HEADER;
    log::error(LOG_NODE) << BN_LOG_HEADER;
    log::fatal(LOG_NODE) << BN_LOG_HEADER;

    const auto& file = metadata_.configured.file;

    if (file.empty())
        log::info(LOG_NODE) << BN_USING_DEFAULT_CONFIG;
    else
        log::info(LOG_NODE) << format(BN_USING_CONFIG_FILE) % file;
}

// ----------------------------------------------------------------------------
// Command line options.
// Emit directly to standard output (not the log).

void executor::do_help()
{
    const auto options = metadata_.load_options();
    printer help(options, application_name, BN_INFORMATION_MESSAGE);
    help.initialize();
    help.commandline(output_);
}

void executor::do_settings()
{
    const auto settings = metadata_.load_settings();
    printer print(settings, application_name, BN_SETTINGS_MESSAGE);
    print.initialize();
    print.settings(output_);
}

void executor::do_version()
{
    output_ << format(BN_VERSION_MESSAGE) %
        LIBBITCOIN_NODE_VERSION %
        LIBBITCOIN_BLOCKCHAIN_VERSION %
        LIBBITCOIN_VERSION << std::endl;
}

// Emit to the logs.
bool executor::do_initchain()
{
    initialize_output();

    error_code ec;
    const auto& directory = metadata_.configured.database.directory;

    if (create_directories(directory, ec))
    {
        log::info(LOG_NODE) << format(BN_INITIALIZING_CHAIN) % directory;

        // Unfortunately we are still limited to a choice of hardcoded chains.
        const auto genesis = metadata_.configured.chain.use_testnet_rules ?
            chain::block::genesis_testnet() : chain::block::genesis_mainnet();

        return data_base::initialize(directory, genesis);
    }

    if (ec.value() == directory_exists)
    {
        log::error(LOG_NODE) << format(BN_INITCHAIN_EXISTS) % directory;
        return false;
    }

    log::error(LOG_NODE) << format(BN_INITCHAIN_NEW) % directory % ec.message();
    return false;
}

// ----------------------------------------------------------------------------
// Invoke an action based on command line option.

bool executor::invoke()
{
    const auto& config = metadata_.configured;

    if (config.help)
    {
        do_help();
        return true;
    }

    if (config.settings)
    {
        do_settings();
        return true;
    }

    if (config.version)
    {
        do_version();
        return true;
    }

    if (config.initchain)
    {
        return do_initchain();
    }

    // There are no command line arguments, just run the node.
    return run();
}

// ----------------------------------------------------------------------------
// Run sequence.

bool executor::run()
{
    initialize_output();
    initialize_interrupt(no_interrupt);

    log::info(LOG_NODE) << BN_NODE_STARTING;

    // Ensure the blockchain directory is initialized (at least exists).
    if (!verify())
        return false;

    // Now that the directory is verified we can create the node for it.
    node_ = std::make_shared<p2p_node>(metadata_.configured);

    // Start seeding the node, stop handlers are registered in start.
    node_->start(
        std::bind(&executor::handle_seeded,
            shared_from_this(), _1));

    log::info(LOG_NODE) << BN_NODE_STARTED;

    // Block until the node is stopped or there is an interrupt.
    return wait_on_stop();
}

void executor::handle_seeded(const code& ec)
{
    if (ec)
    {
        log::error(LOG_NODE) << format(BN_NODE_START_FAIL) % ec.message();
        stopped_ = true;
        return;
    }

    node_->run(
        std::bind(&executor::handle_synchronized,
            shared_from_this(), _1));

    log::info(LOG_NODE) << BN_NODE_SEEDED;
}

void executor::handle_synchronized(const code& ec)
{
    if (ec)
    {
        log::info(LOG_NODE) << format(BN_NODE_START_FAIL) % ec.message();
        stopped_ = true;
        return;
    }

    log::info(LOG_NODE) << BN_NODE_SYNCHRONIZED;
}

// Use missing directory as a sentinel indicating lack of initialization.
bool executor::verify()
{
    error_code ec;
    const auto& directory = metadata_.configured.database.directory;

    if (exists(directory, ec))
        return true;

    if (ec.value() == directory_not_found)
    {
        log::error(LOG_NODE) << format(BN_UNINITIALIZED_CHAIN) % directory;
        return false;
    }

    const auto message = ec.message();
    log::error(LOG_NODE) << format(BN_INITCHAIN_TRY) % directory % message;
    return false;
}

bool executor::wait_on_stop()
{
    std::promise<code> promise;

    // Monitor stopped for completion.
    monitor_stop(
        std::bind(&executor::handle_stopped,
            shared_from_this(), _1, std::ref(promise)));

    // Block until the stop handler is invoked.
    const auto ec = promise.get_future().get();

    if (ec)
    {
        log::error(LOG_NODE) << format(BN_NODE_STOP_FAIL) % ec.message();
        return false;
    }

    log::info(LOG_NODE) << BN_NODE_STOPPED;
    return true;
}

void executor::handle_stopped(const code& ec, std::promise<code>& promise)
{
    promise.set_value(ec);
}

void executor::monitor_stop(p2p::result_handler handler)
{
    while (!stopped_ && !node_->stopped())
        std::this_thread::sleep_for(stop_sensitivity);

    log::info(LOG_NODE) << BN_NODE_UNMAPPING;
    node_->stop(handler);
    node_->close();

    // This is the end of the run sequence.
    node_ = nullptr;
}

} // namespace node
} // namespace libbitcoin
