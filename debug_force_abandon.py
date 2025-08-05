#!/usr/bin/env python3
"""
Debug script for Force Abandon Transaction feature
This script helps troubleshoot why the eviction might be failing
"""

import json
import subprocess
import sys
import os

def run_bitcoin_cli(command, *args):
    """Run bitcoin-cli command and return result"""
    try:
        cmd = ["bitcoin-cli", command] + list(args)
        print(f"Executing: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        print(f"Exit code: {result.returncode}")
        if result.stdout:
            print(f"stdout: {result.stdout.strip()}")
        if result.stderr:
            print(f"stderr: {result.stderr.strip()}")
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except subprocess.TimeoutExpired:
        print("Command timed out")
        return False, "", "Timeout"
    except FileNotFoundError:
        print("bitcoin-cli not found in PATH")
        return False, "", "bitcoin-cli not found"
    except Exception as e:
        print(f"Error: {e}")
        return False, "", str(e)

def check_bitcoin_cli():
    """Check if bitcoin-cli is available and working"""
    print("=== Checking bitcoin-cli availability ===")
    success, stdout, stderr = run_bitcoin_cli("help")
    if success:
        print("✅ bitcoin-cli is working")
        
        # Also test getnetworkinfo to ensure full connectivity
        success, stdout, stderr = run_bitcoin_cli("getnetworkinfo")
        if success:
            print("✅ bitcoin-cli has full connectivity")
        else:
            print("⚠️  bitcoin-cli works but may have connectivity issues")
            print(f"Error: {stderr}")
        
        return True
    else:
        print("❌ bitcoin-cli is not working")
        print(f"Error: {stderr}")
        return False

def check_evicttransaction_rpc():
    """Check if evicttransaction RPC is available"""
    print("\n=== Checking evicttransaction RPC ===")
    success, stdout, stderr = run_bitcoin_cli("help", "evicttransaction")
    if success:
        print("✅ evicttransaction RPC is available")
        print(f"Help: {stdout}")
        return True
    else:
        print("❌ evicttransaction RPC is not available")
        print(f"Error: {stderr}")
        return False

def check_abandontransaction_rpc():
    """Check if abandontransaction RPC is available"""
    print("\n=== Checking abandontransaction RPC ===")
    success, stdout, stderr = run_bitcoin_cli("help", "abandontransaction")
    if success:
        print("✅ abandontransaction RPC is available")
        print(f"Help: {stdout}")
        return True
    else:
        print("❌ abandontransaction RPC is not available")
        print(f"Error: {stderr}")
        return False

def list_unconfirmed_transactions():
    """List unconfirmed transactions"""
    print("\n=== Listing unconfirmed transactions ===")
    success, stdout, stderr = run_bitcoin_cli("listtransactions", "*", "0", "false")
    if success:
        try:
            txs = json.loads(stdout)
            unconfirmed = [tx for tx in txs if tx.get('confirmations', 0) == 0]
            if unconfirmed:
                print(f"Found {len(unconfirmed)} unconfirmed transactions:")
                for tx in unconfirmed:
                    print(f"  TXID: {tx.get('txid', 'N/A')}")
                    print(f"  Category: {tx.get('category', 'N/A')}")
                    print(f"  Amount: {tx.get('amount', 'N/A')}")
                    print(f"  In mempool: {tx.get('involvesWatchonly', False)}")
                    print("  ---")
                return unconfirmed
            else:
                print("No unconfirmed transactions found")
                return []
        except json.JSONDecodeError:
            print("Failed to parse transaction list")
            return []
    else:
        print(f"Failed to list transactions: {stderr}")
        return []

def test_evicttransaction(txid):
    """Test evicttransaction with a specific txid"""
    print(f"\n=== Testing evicttransaction with {txid} ===")
    success, stdout, stderr = run_bitcoin_cli("evicttransaction", txid)
    if success:
        print("✅ evicttransaction succeeded")
        return True
    else:
        print("❌ evicttransaction failed")
        print(f"Error: {stderr}")
        return False

def test_invalid_input_handling():
    """Test how the RPC methods handle invalid input"""
    print("\n=== Testing invalid input handling ===")
    
    # Test evicttransaction with invalid txid
    print("Testing evicttransaction with invalid txid...")
    success, stdout, stderr = run_bitcoin_cli("evicttransaction", "invalid_txid")
    if not success:
        print("✅ evicttransaction correctly rejects invalid input")
    else:
        print("❌ evicttransaction unexpectedly accepted invalid input")
    
    # Test abandontransaction with invalid txid
    print("Testing abandontransaction with invalid txid...")
    success, stdout, stderr = run_bitcoin_cli("abandontransaction", "invalid_txid")
    if not success:
        print("✅ abandontransaction correctly rejects invalid input")
    else:
        print("❌ abandontransaction unexpectedly accepted invalid input")

def check_debug_log():
    """Check if debug.log exists and show recent entries"""
    print("\n=== Checking debug.log ===")
    debug_log_paths = [
        "~/.bitcoin/debug.log",
        "debug.log",
        "/tmp/bitcoin/debug.log"
    ]
    
    for path in debug_log_paths:
        expanded_path = os.path.expanduser(path)
        if os.path.exists(expanded_path):
            print(f"Found debug.log at: {expanded_path}")
            try:
                with open(expanded_path, 'r') as f:
                    lines = f.readlines()
                    recent_lines = lines[-20:]  # Last 20 lines
                    print("Recent debug.log entries:")
                    for line in recent_lines:
                        if any(keyword in line.lower() for keyword in ['evict', 'abandon', 'mempool', 'gui']):
                            print(f"  {line.strip()}")
                return expanded_path
            except Exception as e:
                print(f"Error reading debug.log: {e}")
    
    print("No debug.log found in common locations")
    return None

def main():
    print("🔍 Force Abandon Transaction Debug Tool")
    print("=" * 50)
    
    # Check basic setup
    if not check_bitcoin_cli():
        print("\n❌ Cannot proceed without working bitcoin-cli")
        return
    
    if not check_evicttransaction_rpc():
        print("\n❌ evicttransaction RPC not available - feature may not be compiled")
        return
    
    if not check_abandontransaction_rpc():
        print("\n❌ abandontransaction RPC not available - this is unexpected")
        return
    
    # List unconfirmed transactions
    unconfirmed = list_unconfirmed_transactions()
    
    # Check debug log
    debug_log_path = check_debug_log()
    
    # Test invalid input handling
    test_invalid_input_handling()
    
    # Interactive testing
    if unconfirmed:
        print(f"\n📋 Found {len(unconfirmed)} unconfirmed transactions")
        print("You can test evicttransaction with any of these TXIDs:")
        for i, tx in enumerate(unconfirmed[:5]):  # Show first 5
            txid = tx.get('txid', 'N/A')
            print(f"  {i+1}. {txid}")
        
        try:
            choice = input("\nEnter TXID to test evicttransaction (or press Enter to skip): ").strip()
            if choice:
                test_evicttransaction(choice)
        except KeyboardInterrupt:
            print("\nTest cancelled")
    
    print("\n🔧 Troubleshooting Tips:")
    print("1. Check debug.log for detailed error messages")
    print("2. Ensure bitcoin-cli is in your PATH")
    print("3. Verify RPC connection settings in bitcoin.conf")
    print("4. Check if the transaction is actually in mempool")
    print("5. Ensure you have sufficient permissions")

if __name__ == "__main__":
    main() 