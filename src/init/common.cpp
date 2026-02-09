// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <clientversion.h>
#include <compat/sanity.h>
#include <crypto/sha256.h>
#include <key.h>
#include <logging.h>
#include <node/ui_interface.h>
#include <pubkey.h>
#include <random.h>
#include <util/system.h>
#include <util/time.h>
#include <util/translation.h>

#include <memory>

static std::unique_ptr<ECCVerifyHandle> globalVerifyHandle;

namespace init {
void SetGlobals()
{
    std::string sha256_algo = SHA256AutoDetect();
    LogPrintf("Using the '%s' SHA256 implementation\n", sha256_algo);
    RandomInit();
    ECC_Start();
    globalVerifyHandle.reset(new ECCVerifyHandle());
}

void UnsetGlobals()
{
    globalVerifyHandle.reset();
    ECC_Stop();
}

bool SanityChecks()
{
    if (!ECC_InitSanityCheck()) {
        return InitError(Untranslated("Elliptic curve cryptography sanity check failure. Aborting."));
    }

    if (!glibcxx_sanity_test())
        return false;

    if (!Random_SanityCheck()) {
        return InitError(Untranslated("OS cryptographic RNG sanity check failure. Aborting."));
    }

    if (!ChronoSanityCheck()) {
        return InitError(Untranslated("Clock epoch mismatch. Aborting."));
    }

    return true;
}

void AddLoggingArgs(ArgsManager& argsman)
{
    argsman.AddArg("-debuglogfile=<file>", strprintf("Specify location of debug log file. Relative paths will be prefixed by a net-specific datadir location. (-nodebuglogfile to disable; default: %s)", DEFAULT_DEBUGLOGFILE), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-debug=<category>", "Output debugging information (default: -nodebug, supplying <category> is optional). "
        "If <category> is not supplied or if <category> = 1, output all debugging information. <category> can be: " + LogInstance().LogCategoriesString() + ". This option can be specified multiple times to output multiple categories.",
        ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-debugexclude=<category>", "Exclude debugging information for a category. Can be used in conjunction with -debug=1 to output debug logs for all categories except the specified category. This option can be specified multiple times to exclude multiple categories.", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-logips", strprintf("Include IP addresses in debug output (default: %u)", DEFAULT_LOGIPS), ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-logtimestamps", strprintf("Prepend debug output with timestamp (default: %u)", DEFAULT_LOGTIMESTAMPS), ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
#ifdef HAVE_THREAD_LOCAL
    argsman.AddArg("-logthreadnames", strprintf("Prepend debug output with name of the originating thread (only available on platforms supporting thread_local) (default: %u)", DEFAULT_LOGTHREADNAMES), ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
#else
    argsman.AddHiddenArgs({"-logthreadnames"});
#endif
    argsman.AddArg("-logsourcelocations", strprintf("Prepend debug output with name of the originating source location (source file, line number and function name) (default: %u)", DEFAULT_LOGSOURCELOCATIONS), ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-logtimemicros", strprintf("Add microsecond precision to debug timestamps (default: %u)", DEFAULT_LOGTIMEMICROS), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-logtimeprecision=<n>", strprintf("Set timestamp precision to n decimal places (0-6, default: %d, 0=seconds, 3=milliseconds, 6=microseconds). Use 'auto' to automatically increase precision if timestamps are not unique.", DEFAULT_LOGTIMEPRECISION), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-printtoconsole", "Send trace/debug info to console (default: 1 when no -daemon. To disable logging to file, set -nodebuglogfile)", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-shrinkdebugfile=<n>", "Shrink debug.log file on client startup to the last n MB (default: 10 when no -debug, 0 to disable)", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-rotatelog=<n>", "Rotate debug log file on client startup and keep n versions (default: 0, no rotation)", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
}

void SetLoggingTimePrecision(const ArgsManager& args)
{
    // Parse logtimeprecision option (can be called early, doesn't depend on data directory)
    if (args.IsArgSet("-logtimeprecision")) {
        std::string precision_str = args.GetArg("-logtimeprecision", std::to_string(DEFAULT_LOGTIMEPRECISION));
        if (precision_str == "auto") {
            LogInstance().m_log_time_precision = -1; // Special value for auto mode
            LogInstance().SetAutoPrecision(0); // Start with 0 decimal places for auto mode
        } else {
            try {
                int precision = std::stoi(precision_str);
                if (precision < 0 || precision > 6) {
                    LogPrintf("Warning: -logtimeprecision value %d is out of range (0-6), using default %d\n", precision, DEFAULT_LOGTIMEPRECISION);
                    LogInstance().m_log_time_precision = DEFAULT_LOGTIMEPRECISION;
                } else {
                    LogInstance().m_log_time_precision = precision;
                }
            } catch (const std::exception& e) {
                LogPrintf("Warning: Invalid -logtimeprecision value '%s', using default %d\n", precision_str, DEFAULT_LOGTIMEPRECISION);
                LogInstance().m_log_time_precision = DEFAULT_LOGTIMEPRECISION;
            }
        }
    } else {
        LogInstance().m_log_time_precision = DEFAULT_LOGTIMEPRECISION;
    }
}

void SetLoggingOptions(const ArgsManager& args)
{
    LogInstance().m_print_to_file = !args.IsArgNegated("-debuglogfile");
    LogInstance().m_file_path = AbsPathForConfigVal(fs::PathFromString(args.GetArg("-debuglogfile", DEFAULT_DEBUGLOGFILE)));
    LogInstance().m_print_to_console = args.GetBoolArg("-printtoconsole", !args.GetBoolArg("-daemon", false));
    LogInstance().m_log_timestamps = args.GetBoolArg("-logtimestamps", DEFAULT_LOGTIMESTAMPS);
    LogInstance().m_log_time_micros = args.GetBoolArg("-logtimemicros", DEFAULT_LOGTIMEMICROS);
#ifdef HAVE_THREAD_LOCAL
    LogInstance().m_log_threadnames = args.GetBoolArg("-logthreadnames", DEFAULT_LOGTHREADNAMES);
#endif
    LogInstance().m_log_sourcelocations = args.GetBoolArg("-logsourcelocations", DEFAULT_LOGSOURCELOCATIONS);

    fLogIPs = args.GetBoolArg("-logips", DEFAULT_LOGIPS);
}

void SetLoggingCategories(const ArgsManager& args)
{
    if (args.IsArgSet("-debug")) {
        // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
        const std::vector<std::string> categories = args.GetArgs("-debug");

        if (std::none_of(categories.begin(), categories.end(),
            [](std::string cat){return cat == "0" || cat == "none";})) {
            for (const auto& cat : categories)
                LogInstance().EnableCategory(cat);
        }
    }

    // Now remove the logging categories which were explicitly excluded
    for (const std::string& cat : args.GetArgs("-debugexclude"))
        LogInstance().DisableCategory(cat);
}

bool StartLogging(const ArgsManager& args)
{
    if (LogInstance().m_print_to_file) {
        int64_t rotate_logs = args.GetIntArg("-rotatelog", 0);
        if (rotate_logs < 0) {
            LogPrintf("Warning: -rotatelog value %lld is invalid, using 0\n", static_cast<long long>(rotate_logs));
            rotate_logs = 0;
        }
        if (rotate_logs > 0) {
            int64_t keep_old = rotate_logs - 1;
            if (keep_old == 0) {
                try {
                    if (fs::exists(LogInstance().m_file_path)) {
                        fs::remove(LogInstance().m_file_path);
                    }
                } catch (const fs::filesystem_error& e) {
                    LogPrintf("Failed to rotate debug log file: %s\n", e.what());
                }
            } else {
                LogInstance().RotateDebugFile(keep_old);
            }
        } else {
            int64_t shrink_mb = 0;
            if (args.IsArgSet("-shrinkdebugfile")) {
                shrink_mb = args.GetIntArg("-shrinkdebugfile", 0);
            } else if (LogInstance().DefaultShrinkDebugFile()) {
                shrink_mb = 10;
            }
            if (shrink_mb < 0) {
                LogPrintf("Warning: -shrinkdebugfile value %lld is invalid, using 0\n", static_cast<long long>(shrink_mb));
                shrink_mb = 0;
            }
            if (shrink_mb > 0) {
                // Do this first since it both loads a bunch of debug.log into memory,
                // and because this needs to happen before any other debug.log printing
                LogInstance().ShrinkDebugFile(shrink_mb);
            }
        }
    }
    if (!LogInstance().StartLogging()) {
            return InitError(strprintf(Untranslated("Could not open debug log file %s"),
                fs::PathToString(LogInstance().m_file_path)));
    }

    if (!LogInstance().m_log_timestamps)
        LogPrintf("Startup time: %s\n", FormatISO8601DateTime(GetTime()));
    LogPrintf("Default data directory %s\n", fs::PathToString(GetDefaultDataDir()));
    LogPrintf("Using data directory %s\n", fs::PathToString(gArgs.GetDataDirNet()));

    // Only log conf file usage message if conf file actually exists.
    fs::path config_file_path = GetConfigFile(args.GetArg("-conf", BITCOIN_CONF_FILENAME));
    if (fs::exists(config_file_path)) {
        LogPrintf("Config file: %s\n", fs::PathToString(config_file_path));
    } else if (args.IsArgSet("-conf")) {
        // Warn if no conf file exists at path provided by user
        InitWarning(strprintf(_("The specified config file %s does not exist"), fs::PathToString(config_file_path)));
    } else {
        // Not categorizing as "Warning" because it's the default behavior
        LogPrintf("Config file: %s (not found, skipping)\n", fs::PathToString(config_file_path));
    }

    // Log the config arguments to debug.log
    args.LogArgs();

    return true;
}

void LogPackageVersion()
{
    std::string version_string = FormatFullVersion();
#ifdef DEBUG
    version_string += " (debug build)";
#else
    version_string += " (release build)";
#endif
    LogPrintf(PACKAGE_NAME " version %s\n", version_string);
}
} // namespace init
