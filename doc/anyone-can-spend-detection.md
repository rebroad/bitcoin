# Anyone-Can-Spend Detection: Current Behavior and Expansion Options

This document describes how the current code identifies "anyone-can-spend" outputs, where it acts on them, and what additional detection checks are possible.

## Scope and Definition

In this codebase, an output is treated as anyone-can-spend if we can construct a valid base-script spend path without private keys or wallet-owned secrets.

Important safety constraint:
- Witness programs are explicitly excluded (`scriptPubKey.IsWitnessProgram(...) == true`), which avoids accidentally classifying SegWit v0/v1/v2+ (including Taproot) as anyone-can-spend.

## Current Detection Pipeline

Primary implementation:
- `src/anyone_can_spend_handler.cpp`
- `src/anyone_can_spend_handler.h`

Shared classifier:
- `anyonecanspend::IsAnyoneCanSpendScriptPubKey(const CScript&, CScript* spend_script_sig)`
- `anyonecanspend::FindAnyoneCanSpendOutputs(const CTransaction&, size_t max_outputs)`

Current checks:
1. Reject witness program outputs early.
2. Fast-path match for known trivial scripts:
   - `OP_TRUE` / `OP_1`
   - `OP_DROP OP_TRUE` / `OP_DROP OP_1` (with a minimal push in scriptSig)
3. Probe-based evaluation for a small set of candidate scriptSigs:
   - empty
   - `OP_1`
   - `OP_0`
   - `OP_1 OP_1`
4. Evaluate scriptSig then scriptPubKey with `EvalScript(..., SigVersion::BASE, STANDARD_SCRIPT_VERIFY_FLAGS, ...)`.
5. If execution succeeds and top stack value is true, classify as anyone-can-spend and return the matching scriptSig template.

Notes:
- Probe execution is marked via `ScriptExecutionData::m_anyone_can_spend_probe = true`.
- `EvalScript` "NOT EQUAL" debug logs are suppressed only for probe executions to avoid log flooding.

## When It Acts

`AnyoneCanSpendHandler` is notified on:
- mempool arrival (`TransactionAddedToMempool`)
- connected blocks (`BlockConnected`)

If initialized and auto-spend is enabled, detected outputs are added to the `Anyone` wallet flow and may be auto-spent to `-anyonecanspenddestination`.

## Fee Policy for Auto-Spend

Auto-spend uses a conservative "next block" target with a floor:
- confirm target: 1 block
- fee estimate mode: conservative
- enforced minimum floor: 20 sat/vB
- final rate is max(estimated, relay floor, mempool floor, 20 sat/vB floor)

## What Additional Spendable Outputs Could Be Detected?

Yes, coverage can be widened. Options below are ordered roughly from low to high complexity.

### Option A: Expand candidate scriptSig templates

Idea:
- Test more short push-only scriptSigs (e.g. more combinations of empty/0/1/small data pushes).

Upside:
- Detects more "practically spendable by anyone" legacy scripts.

Downside:
- Runtime cost increases with candidate count.
- Higher false-positive risk if templates accidentally satisfy scripts that are not broadly spendable in practice.

### Option B: Two-checker probing (true-checker and false-checker)

Idea:
- Run probes twice:
  - checker that returns true for sig/locktime/sequence checks
  - checker that returns false for those checks
- Only accept outputs if a candidate succeeds even with the strict/false checker, or if policy explicitly allows "context-dependent spendable now".

Upside:
- Reduces false positives from scripts that only pass because probe checker accepted signatures or timelocks.

Downside:
- Might miss outputs that are spendable by anyone "now" but depend on current locktime/sequence context.
- More CPU than current single-checker probing.

### Option C: Bounded search over small stack inputs

Idea:
- Instead of fixed templates, perform bounded exploration over small push-data combinations (depth/size limits).

Upside:
- Better coverage for unusual but still simple non-witness scripts.

Downside:
- Search-space growth can become expensive quickly.
- Harder to reason about worst-case performance.

### Option D: Distinguish "consensus-spendable" vs "relay-spendable"

Idea:
- Keep script-level detection, then preflight candidate spend transaction with mempool policy acceptance (standardness/minrelay).

Upside:
- Prevents attempts that are consensus-valid but unlikely to relay.

Downside:
- Adds runtime dependency on current node mempool policy and conditions.
- Can vary across peers/miners.

### Option E: Add explicit categories

Idea:
- Emit classification categories:
  - `unconditional-anyone-can-spend`
  - `context-dependent-spendable-now`
  - `consensus-spendable-but-nonstandard-risk`

Upside:
- Operational clarity and better debugging/telemetry.

Downside:
- More code/maintenance and API surface.

## Recommended Direction

If we want broader detection without pulling in newer protocols:
1. Keep witness exclusion as-is (do not include SegWit/Taproot paths).
2. Add Option B (two-checker probing) to reduce false positives.
3. Carefully widen templates (Option A) with tight limits and telemetry.
4. Gate auto-spend by policy preflight (Option D) before commit.

This keeps behavior conservative while improving detection coverage for truly spendable legacy outputs.
