// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/server_util.h>
#include <anyone_can_spend_handler.h>
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

// Global handler instance
static std::unique_ptr<AnyoneCanSpendHandler> g_anyone_can_spend_handler;

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

        // Create the handler if it doesn't exist
        if (!g_anyone_can_spend_handler) {
            g_anyone_can_spend_handler = std::make_unique<AnyoneCanSpendHandler>();
        }

        // Initialize the handler
        g_anyone_can_spend_handler->Initialize(destination, node.wallet_loader->context(), false); // Start with auto-spend disabled

        result.pushKV("status", "initialized");
        result.pushKV("destination", destination);
        result.pushKV("auto_spend", false);

    } else if (command == "enable") {
        if (!g_anyone_can_spend_handler) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Handler not initialized. Use 'init' first.");
        }

        g_anyone_can_spend_handler->SetAutoSpend(true);

        result.pushKV("status", "auto-spend enabled");
        result.pushKV("auto_spend", true);
        result.pushKV("destination", g_anyone_can_spend_handler->GetDestinationAddress());

    } else if (command == "disable") {
        if (!g_anyone_can_spend_handler) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Handler not initialized. Use 'init' first.");
        }

        g_anyone_can_spend_handler->SetAutoSpend(false);

        result.pushKV("status", "auto-spend disabled");
        result.pushKV("auto_spend", false);
        result.pushKV("destination", g_anyone_can_spend_handler->GetDestinationAddress());

    } else if (command == "status") {
        if (!g_anyone_can_spend_handler) {
            result.pushKV("status", "not initialized");
            result.pushKV("auto_spend", false);
            result.pushKV("destination", "");
            result.pushKV("config_destination", gArgs.GetArg("-anyonecanspenddestination", ""));
        } else {
            result.pushKV("status", "initialized");
            result.pushKV("auto_spend", g_anyone_can_spend_handler->GetAutoSpend());
            result.pushKV("destination", g_anyone_can_spend_handler->GetDestinationAddress());
            result.pushKV("config_destination", gArgs.GetArg("-anyonecanspenddestination", ""));
        }
    } else if (command == "stats") {
        if (!g_anyone_can_spend_handler) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Handler not initialized. Use 'init' first.");
        }

        auto stats = g_anyone_can_spend_handler->GetStats();

        UniValue stats_obj(UniValue::VOBJ);
        stats_obj.pushKV("outputs_detected", (int64_t)stats.outputs_detected);
        stats_obj.pushKV("outputs_spent", (int64_t)stats.outputs_spent);
        stats_obj.pushKV("total_amount_detected", FormatMoney(stats.total_amount_detected));
        stats_obj.pushKV("total_amount_spent", FormatMoney(stats.total_amount_spent));

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
