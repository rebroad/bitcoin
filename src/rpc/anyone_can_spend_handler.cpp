// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/server_util.h>
#include <anyone_can_spend_handler.h>
#include <init.h>
#include <node/context.h>
#include <util/strencodings.h>
#include <key_io.h>
#include <script/standard.h>
#include <util/moneystr.h>
#include <rpc/util.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <node/context.h>
#include <interfaces/wallet.h>

#include <univalue.h>

static RPCHelpMan anyonecanspendhandler()
{
    return RPCHelpMan{"anyonecanspendhandler",
                "\nControl the anyone can spend output handler.\n"
                "This allows you to automatically spend 'anyone can spend' outputs to a specified address.\n"
                "The destination address can be specified in bitcoin.conf with -anyonecanspenddestination=<address>\n",
                {
                    {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "The command to execute", "init|status|enable|disable|stats"},
                    {"destination", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Destination address for spending outputs (optional, can use config setting)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "status", "Status of the operation"},
                        {RPCResult::Type::BOOL, "auto_spend", "Whether auto-spending is enabled"},
                        {RPCResult::Type::STR, "destination", "Destination address for spending"},
                        {RPCResult::Type::STR, "config_destination", "Destination address from configuration"},
                        {RPCResult::Type::OBJ, "stats", "Statistics about handled outputs",
                        {
                            {RPCResult::Type::NUM, "outputs_detected", "Number of outputs detected"},
                            {RPCResult::Type::NUM, "outputs_spent", "Number of outputs spent"},
                            {RPCResult::Type::STR_AMOUNT, "total_amount_detected", "Total amount detected"},
                            {RPCResult::Type::STR_AMOUNT, "total_amount_spent", "Total amount spent"},
                            {RPCResult::Type::NUM, "transactions_evaluated_mempool", "Transactions evaluated from mempool callbacks"},
                            {RPCResult::Type::NUM, "transactions_evaluated_blocks", "Transactions evaluated from block callbacks"},
                            {RPCResult::Type::NUM, "cleanup_runs", "Number of stale-transaction cleanup runs"},
                            {RPCResult::Type::NUM, "cleanup_removed", "Number of stale transactions removed by cleanup"},
                            {RPCResult::Type::OBJ, "runtime", "Adaptive runtime metrics",
                            {
                                {RPCResult::Type::NUM, "max_outputs_scanned_per_tx", "Current output scan limit per tx"},
                                {RPCResult::Type::NUM, "max_outputs_returned_per_tx", "Current detection cap per tx"},
                                {RPCResult::Type::NUM, "max_probe_evals_per_tx", "Current script eval budget per tx"},
                                {RPCResult::Type::NUM, "max_probe_script_sig_templates", "Current scriptSig template budget"},
                                {RPCResult::Type::NUM, "ema_elapsed_us_mempool", "EMA callback runtime (microseconds) for mempool path"},
                                {RPCResult::Type::NUM, "ema_elapsed_us_block", "EMA callback runtime (microseconds) for block path"},
                                {RPCResult::Type::NUM, "mempool_events", "Observed mempool callback events"},
                                {RPCResult::Type::NUM, "block_events", "Observed block callback tx evaluations"},
                                {RPCResult::Type::NUM, "mempool_budget_exhaustions", "Times mempool path exhausted probe budget"},
                                {RPCResult::Type::NUM, "block_budget_exhaustions", "Times block path exhausted probe budget"},
                                {RPCResult::Type::NUM, "mempool_overruns", "Times mempool callback exceeded target runtime"},
                                {RPCResult::Type::NUM, "block_overruns", "Times block callback exceeded target runtime"},
                                {RPCResult::Type::NUM, "cache_hits", "Script classification cache hits"},
                                {RPCResult::Type::NUM, "cache_misses", "Script classification cache misses"},
                                {RPCResult::Type::NUM, "cache_hit_rate_percent", "Script classification cache hit rate"},
                                {RPCResult::Type::NUM, "target_elapsed_us_mempool", "Mempool callback runtime target (microseconds)"},
                                {RPCResult::Type::NUM, "target_elapsed_us_block", "Block callback runtime target (microseconds)"},
                                {RPCResult::Type::NUM, "adjust_interval_events", "Events between adaptive tuning adjustments"},
                                {RPCResult::Type::NUM, "ema_alpha", "EMA alpha used for runtime smoothing"},
                                {RPCResult::Type::NUM, "overload_exhaust_threshold", "Exhaust-rate threshold triggering overload behavior"},
                                {RPCResult::Type::NUM, "underload_exhaust_threshold", "Exhaust-rate threshold for underload growth"},
                                {RPCResult::Type::NUM, "overload_scale_percent", "Scale factor (%) applied when overloaded"},
                                {RPCResult::Type::NUM, "underload_scale_percent", "Scale factor (%) applied when underloaded"},
                                {RPCResult::Type::NUM, "cleanup_interval_secs", "Cleanup run interval in seconds"},
                                {RPCResult::Type::NUM, "cleanup_stale_age_secs", "Unconfirmed transaction age threshold before cleanup"},
                                {RPCResult::Type::NUM, "cleanup_max_candidates", "Max stale candidates evaluated per cleanup run"},
                            }},
                        }},
                    }},
                RPCExamples{
                    HelpExampleCli("anyonecanspendhandler", "init \"bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh\"")
            + HelpExampleCli("anyonecanspendhandler", "init")
            + HelpExampleCli("anyonecanspendhandler", "enable")
            + HelpExampleCli("anyonecanspendhandler", "status")
            + HelpExampleCli("anyonecanspendhandler", "stats")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string command = request.params[0].get_str();

    UniValue result(UniValue::VOBJ);

    if (command == "init") {
        std::string destination;

        if (request.params.size() >= 2) {
            destination = request.params[1].get_str();
        } else {
            // Try to get destination from configuration
            destination = gArgs.GetArg("-anyonecanspenddestination", "");
            if (destination.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    "No destination address provided and none configured in bitcoin.conf. "
                    "Use -anyonecanspenddestination=<address> in bitcoin.conf or provide as parameter.");
            }
        }

        // Validate the destination address
        CTxDestination dest = DecodeDestination(destination);
        if (std::holds_alternative<CNoDestination>(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid destination address: " + destination);
        }

        // Get the wallet context
        const node::NodeContext& node = EnsureAnyNodeContext(request.context);
        if (!node.wallet_loader) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wallet loader not available");
        }

        AnyoneCanSpendHandler& handler = EnsureAnyoneCanSpendHandler();

        // Initialize the handler
        handler.Initialize(destination, node.wallet_loader->context(), false); // Start with auto-spend disabled

        result.pushKV("status", "initialized");
        result.pushKV("destination", destination);
        result.pushKV("auto_spend", false);

    } else if (command == "enable") {
        auto* handler = GetAnyoneCanSpendHandler();
        if (!handler) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Handler not initialized. Use 'init' first.");
        }

        handler->SetAutoSpend(true);

        result.pushKV("status", "auto-spend enabled");
        result.pushKV("auto_spend", true);
        result.pushKV("destination", handler->GetDestinationAddress());

    } else if (command == "disable") {
        auto* handler = GetAnyoneCanSpendHandler();
        if (!handler) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Handler not initialized. Use 'init' first.");
        }

        handler->SetAutoSpend(false);

        result.pushKV("status", "auto-spend disabled");
        result.pushKV("auto_spend", false);
        result.pushKV("destination", handler->GetDestinationAddress());

    } else if (command == "status") {
        auto* handler = GetAnyoneCanSpendHandler();
        if (!handler) {
            result.pushKV("status", "not initialized");
            result.pushKV("auto_spend", false);
            result.pushKV("destination", "");
            result.pushKV("config_destination", gArgs.GetArg("-anyonecanspenddestination", ""));
        } else {
            result.pushKV("status", "initialized");
            result.pushKV("auto_spend", handler->GetAutoSpend());
            result.pushKV("destination", handler->GetDestinationAddress());
            result.pushKV("config_destination", gArgs.GetArg("-anyonecanspenddestination", ""));
        }
    } else if (command == "stats") {
        auto* handler = GetAnyoneCanSpendHandler();
        if (!handler) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Handler not initialized. Use 'init' first.");
        }

        auto stats = handler->GetStats();
        auto runtime = handler->GetRuntimeMetrics();

        UniValue stats_obj(UniValue::VOBJ);
        stats_obj.pushKV("outputs_detected", (int64_t)stats.outputs_detected);
        stats_obj.pushKV("outputs_spent", (int64_t)stats.outputs_spent);
        stats_obj.pushKV("total_amount_detected", FormatMoney(stats.total_amount_detected));
        stats_obj.pushKV("total_amount_spent", FormatMoney(stats.total_amount_spent));
        stats_obj.pushKV("transactions_evaluated_mempool", (int64_t)stats.transactions_evaluated_mempool);
        stats_obj.pushKV("transactions_evaluated_blocks", (int64_t)stats.transactions_evaluated_blocks);
        stats_obj.pushKV("cleanup_runs", (int64_t)stats.cleanup_runs);
        stats_obj.pushKV("cleanup_removed", (int64_t)stats.cleanup_removed);

        UniValue runtime_obj(UniValue::VOBJ);
        runtime_obj.pushKV("max_outputs_scanned_per_tx", (int64_t)runtime.max_outputs_scanned_per_tx);
        runtime_obj.pushKV("max_outputs_returned_per_tx", (int64_t)runtime.max_outputs_returned_per_tx);
        runtime_obj.pushKV("max_probe_evals_per_tx", (int64_t)runtime.max_probe_evals_per_tx);
        runtime_obj.pushKV("max_probe_script_sig_templates", (int64_t)runtime.max_probe_script_sig_templates);
        runtime_obj.pushKV("ema_elapsed_us_mempool", runtime.ema_elapsed_us_mempool);
        runtime_obj.pushKV("ema_elapsed_us_block", runtime.ema_elapsed_us_block);
        runtime_obj.pushKV("mempool_events", (int64_t)runtime.mempool_events);
        runtime_obj.pushKV("block_events", (int64_t)runtime.block_events);
        runtime_obj.pushKV("mempool_budget_exhaustions", (int64_t)runtime.mempool_budget_exhaustions);
        runtime_obj.pushKV("block_budget_exhaustions", (int64_t)runtime.block_budget_exhaustions);
        runtime_obj.pushKV("mempool_overruns", (int64_t)runtime.mempool_overruns);
        runtime_obj.pushKV("block_overruns", (int64_t)runtime.block_overruns);
        runtime_obj.pushKV("cache_hits", (int64_t)runtime.cache_hits);
        runtime_obj.pushKV("cache_misses", (int64_t)runtime.cache_misses);
        const double cache_hit_rate = (runtime.cache_hits + runtime.cache_misses) > 0
            ? (100.0 * runtime.cache_hits) / (runtime.cache_hits + runtime.cache_misses)
            : 0.0;
        runtime_obj.pushKV("cache_hit_rate_percent", cache_hit_rate);
        runtime_obj.pushKV("target_elapsed_us_mempool", runtime.target_elapsed_us_mempool);
        runtime_obj.pushKV("target_elapsed_us_block", runtime.target_elapsed_us_block);
        runtime_obj.pushKV("adjust_interval_events", (int64_t)runtime.adjust_interval_events);
        runtime_obj.pushKV("ema_alpha", runtime.ema_alpha);
        runtime_obj.pushKV("overload_exhaust_threshold", runtime.overload_exhaust_threshold);
        runtime_obj.pushKV("underload_exhaust_threshold", runtime.underload_exhaust_threshold);
        runtime_obj.pushKV("overload_scale_percent", runtime.overload_scale_percent);
        runtime_obj.pushKV("underload_scale_percent", runtime.underload_scale_percent);
        runtime_obj.pushKV("cleanup_interval_secs", runtime.cleanup_interval_secs);
        runtime_obj.pushKV("cleanup_stale_age_secs", runtime.cleanup_stale_age_secs);
        runtime_obj.pushKV("cleanup_max_candidates", runtime.cleanup_max_candidates);
        stats_obj.pushKV("runtime", runtime_obj);

        result.pushKV("status", "stats retrieved");
        result.pushKV("stats", stats_obj);

    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid command. Use 'init', 'enable', 'disable', 'status', or 'stats'");
    }

    return result;
},
    };
}

void RegisterAnyoneCanSpendHandlerRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[] = {
        {"anyonecanspend", &anyonecanspendhandler},
    };

    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
