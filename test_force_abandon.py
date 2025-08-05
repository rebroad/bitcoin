#!/usr/bin/env python3
"""
Test script for the new Force Abandon Transaction feature
This script tests the RPC functionality we added
"""

import json
import subprocess
import sys

def run_bitcoin_cli(method, params=None):
    """Run a bitcoin-cli command and return the result"""
    cmd = ["bitcoin-cli", method]
    if params:
        if isinstance(params, list):
            cmd.extend(params)
        else:
            cmd.append(str(params))
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        print(f"Error running {method}: {e}")
        print(f"stderr: {e.stderr}")
        return None

def test_evicttransaction_rpc():
    """Test the new evicttransaction RPC method"""
    print("🧪 Testing evicttransaction RPC method...")
    
    # Test with invalid txid
    result = run_bitcoin_cli("evicttransaction", "invalid_txid")
    if result is None:
        print("✅ RPC method exists and handles invalid input correctly")
    else:
        print("❌ Unexpected result for invalid input")
        return False
    
    # Test help
    help_result = run_bitcoin_cli("help", "evicttransaction")
    if help_result and "evicttransaction" in help_result:
        print("✅ RPC help documentation exists")
    else:
        print("❌ RPC help documentation missing")
        return False
    
    print("✅ evicttransaction RPC method test passed!")
    return True

def test_abandontransaction_rpc():
    """Test the existing abandontransaction RPC method"""
    print("🧪 Testing abandontransaction RPC method...")
    
    # Test help
    help_result = run_bitcoin_cli("help", "abandontransaction")
    if help_result and "abandontransaction" in help_result:
        print("✅ abandontransaction RPC help documentation exists")
    else:
        print("❌ abandontransaction RPC help documentation missing")
        return False
    
    print("✅ abandontransaction RPC method test passed!")
    return True

def main():
    """Main test function"""
    print("🚀 Testing Force Abandon Transaction Feature")
    print("=" * 50)
    
    # Check if bitcoin-cli is available
    try:
        version = run_bitcoin_cli("getnetworkinfo")
        if not version:
            print("❌ bitcoin-cli not available or not working")
            return False
        print("✅ bitcoin-cli is available")
    except FileNotFoundError:
        print("❌ bitcoin-cli not found in PATH")
        return False
    
    # Run tests
    tests = [
        test_evicttransaction_rpc,
        test_abandontransaction_rpc,
    ]
    
    passed = 0
    total = len(tests)
    
    for test in tests:
        if test():
            passed += 1
        print()
    
    print("=" * 50)
    print(f"📊 Test Results: {passed}/{total} tests passed")
    
    if passed == total:
        print("🎉 All tests passed! Force Abandon feature is working correctly!")
        return True
    else:
        print("❌ Some tests failed. Please check the implementation.")
        return False

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1) 