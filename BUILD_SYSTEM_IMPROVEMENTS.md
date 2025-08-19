# Bitcoin Core Build System Improvements

This document describes the enhanced build system features that help developers debug and manage build issues more effectively.

## Quick Start

To see all available custom build targets:
```bash
make help-build
```

## When to Use These Targets

### You're Getting "Undefined Reference" Errors

If you see errors like:
```
undefined reference to `wallet::CWallet::IsMine'
undefined reference to `crypto::SHA256'
```

**Try these solutions in order:**

1. **Check if libraries are built correctly:**
   ```bash
   make verify-build
   ```

2. **If verification fails, force rebuild the specific library:**
   ```bash
   # For wallet errors:
   make force-rebuild-wallet
   
   # For crypto errors:
   make force-rebuild-crypto
   
   # For consensus errors:
   make force-rebuild-consensus
   ```

3. **If still having issues, try the nuclear option:**
   ```bash
   make force-rebuild-all
   ```

### You're Getting "Malformed Archive" Errors

If you see errors like:
```
malformed archive
```

**Try these solutions:**

1. **Clean all library artifacts and rebuild:**
   ```bash
   make clean-libs && make -j8
   ```

2. **If that doesn't work, force rebuild everything:**
   ```bash
   make force-rebuild-all
   ```

### Regular `make clean` Isn't Fixing Your Issues

Sometimes the standard `make clean` doesn't clear all dependency files or object files that are causing issues.

**Try these granular clean targets:**

```bash
# Clean specific library only:
make clean-wallet

# Clean all libraries:
make clean-libs

# Then rebuild:
make -j8
```

## Available Targets

### Verification Targets

These targets check if libraries contain the expected object files:

- `verify-wallet` - Verify wallet library
- `verify-crypto` - Verify crypto library  
- `verify-consensus` - Verify consensus library
- `verify-common` - Verify common library
- `verify-util` - Verify util library
- `verify-build` - Verify ALL libraries (recommended)

### Force Rebuild Targets

These targets force a complete rebuild of specific libraries:

- `force-rebuild-wallet` - Rebuild wallet library
- `force-rebuild-crypto` - Rebuild crypto library
- `force-rebuild-consensus` - Rebuild consensus library
- `force-rebuild-common` - Rebuild common library
- `force-rebuild-util` - Rebuild util library
- `force-rebuild-all` - Rebuild ALL libraries (nuclear option)

### Clean Targets

These targets remove build artifacts:

- `clean-wallet` - Clean wallet artifacts only
- `clean-crypto` - Clean crypto artifacts only
- `clean-consensus` - Clean consensus artifacts only
- `clean-common` - Clean common artifacts only
- `clean-util` - Clean util artifacts only
- `clean-libs` - Clean ALL library artifacts

## Common Workflows

### Debugging Build Issues

```bash
# 1. Check if libraries are built correctly
make verify-build

# 2. If verification fails, identify which library is problematic
# Look at the error messages to see which library is missing symbols

# 3. Force rebuild the problematic library
make force-rebuild-wallet  # or crypto, consensus, etc.

# 4. Verify again
make verify-build

# 5. If still failing, try the nuclear option
make force-rebuild-all
```

### After Making Changes to Source Files

```bash
# If you modified wallet code:
make force-rebuild-wallet

# If you modified crypto code:
make force-rebuild-crypto

# If you're unsure what you modified:
make verify-build
```

### When Switching Branches or After Pull

```bash
# Clean everything and rebuild
make clean-libs && make -j8

# Or if you want to be extra sure:
make force-rebuild-all
```

## Background

These improvements were added to address common build issues in Bitcoin Core development, particularly:

1. **Segmentation faults** caused by invalid iterator usage in wallet code
2. **Missing object files** in library archives
3. **Dependency tracking issues** where `make clean` doesn't clear all problematic files
4. **"Malformed archive" errors** from corrupted library files

The targets provide granular control over the build process, making it easier to debug and fix build issues without having to rebuild the entire project.

## Integration with Existing Workflow

These targets complement the existing build system:

- They don't interfere with normal `make` commands
- They follow the same patterns as existing targets
- They can be used alongside standard automake targets
- They provide additional debugging capabilities without changing the core build process

## Getting Help

To see all available targets with descriptions:
```bash
make help-build
```

For more detailed information about the build system, see the main Bitcoin Core documentation.
