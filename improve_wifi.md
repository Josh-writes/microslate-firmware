# WiFi Connection Improvement Plan

## Problem
The current firmware supports storing up to 4 WiFi network credentials. However, the `wifiSyncStart` function implements a "shortcut" that blindly attempts to auto-connect to the first saved network in the NVS storage without verifying if that network is actually available.

This leads to a poor user experience when a user has multiple saved networks (e.g., Home and Work). If the Home network was the first one saved, the device will attempt to connect to it even when the user is at Work, resulting in a connection timeout and a frustrating loop before the user can manually select the Work network.

## Proposed Solution
Transition from "blind auto-connect" to "intelligent auto-connect" based on real-time environment scanning.

### Changes
1. **Disable Blind Auto-Connect**: Remove the call to `getFirstSavedCredential` and the subsequent `beginConnect` inside `wifiSyncStart`. The process should always begin with `beginScan()`.
2. **Intelligent Selection**: Modify `processScanResults()` to leverage the existing sorting logic. Since `processScanResults` already sorts networks with saved credentials to the top of the list (and then by signal strength), the first entry in the `networks` array will always be the strongest available saved network.
3. **Automatic Transition**: In `processScanResults()`, check if `networks[0].saved` is true. If it is, automatically call `beginConnect()` for that network.
4. **Manual Fallback**: If no detected networks have saved credentials, the device will remain in the `NETWORK_LIST` state, allowing the user to choose a network manually.

## Expected Outcome
The device will automatically connect to the strongest known WiFi network available in its current environment, eliminating connection loops when moving between different saved locations.
