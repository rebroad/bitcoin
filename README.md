Bitcoin Core integration/staging tree
=====================================

https://bitcoincore.org

For an immediately usable, binary version of the Bitcoin Core software, see
https://bitcoincore.org/en/download/.

Further information about Bitcoin Core is available in the [doc folder](/doc).

What is Bitcoin?
----------------

Bitcoin is an experimental digital currency that enables instant payments to
anyone, anywhere in the world. Bitcoin uses peer-to-peer technology to operate
with no central authority: managing transactions and issuing money are carried
out collectively by the network. Bitcoin Core is the name of open source
software which enables the use of this currency.

For more information read the original Bitcoin whitepaper.

License
-------

Bitcoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built (see `doc/build-*.md` for instructions) and tested, but it is not guaranteed to be
completely stable. [Tags](https://github.com/bitcoin/bitcoin/tags) are created
regularly from release branches to indicate new official, stable release versions of Bitcoin Core.

The https://github.com/bitcoin-core/gui repository is used exclusively for the
development of the GUI. Its master branch is identical in all monotree
repositories. Release branches and tags do not exist, so please do not fork
that repository unless it is for development reasons.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md)
and useful hints for developers can be found in [doc/developer-notes.md](doc/developer-notes.md).

Build System
------------

This project includes enhanced build system features with automatic error detection and recovery to help developers debug and manage build issues more effectively.

### Automatic Build Recovery

The build system now automatically detects and fixes common build issues:

- **Corrupted libraries** are automatically detected and removed
- **Undefined reference errors** are automatically identified and fixed
- **Build failures** trigger helpful error messages with suggested fixes

### Quick Help

To see all available custom build targets:
```bash
make help-build
```

### Smart Build Options

- `make smart-build` - Build with automatic error recovery
- `make robust-build` - Build with corrupted library detection
- `./build-aux/build-recovery.sh` - Build with intelligent error analysis

### Manual Recovery Targets

If automatic recovery doesn't work, these manual targets are available:

- `make fix-undefined-refs` - Fix undefined reference errors (most common)
- `make fix-build` - Comprehensive build fix
- `make rebuild-libs` - Force rebuild all libraries

### Common Build Issues

If you encounter build problems like "undefined reference" errors or "malformed archive" errors, the system will automatically suggest the appropriate fix. For detailed troubleshooting steps, see [BUILD_SYSTEM_IMPROVEMENTS.md](BUILD_SYSTEM_IMPROVEMENTS.md).

Key targets for common issues:
- `make verify-build` - Check if all libraries are built correctly
- `make force-rebuild-wallet` - Force rebuild wallet library (for wallet-related errors)
- `make clean-libs && make -j8` - Clean libraries and rebuild (when `make clean` isn't enough)

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled in configure) with: `make check`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python.
These tests can be run (if the [test dependencies](/test) are installed) with: `test/functional/test_runner.py`

The CI (Continuous Integration) systems make sure that every pull request is built for Windows, Linux, and macOS,
and that unit/sanity tests are run automatically.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

Changes to translations as well as new translations can be submitted to
[Bitcoin Core's Transifex page](https://www.transifex.com/bitcoin/bitcoin/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.
