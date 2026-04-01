# mod-gold-rush

`mod-gold-rush` is an AzerothCore module that runs a timed, server-wide
resource eruption event. When Gold Rush activates, the module selects an
eligible open-world zone, anchors on a live player, spawns temporary resource
nodes near the hotspot, announces the event to the server, and can route
playerbots into the area to create a PvPvE scramble.

## Requirements

> **Required dependency:** `mod-playerbots` must be installed and enabled.
> Gold Rush includes direct playerbot integration and will not work correctly
> without the Playerbots module.

- Requires AzerothCore.
- Requires `mod-playerbots`.
- Gold Rush bot routing, hotspot pressure, and related behavior depend on
  Playerbots APIs.

## Features

- Schedules Gold Rush events on a random interval.
- Announces an active hotspot with approximate location messaging.
- Spawns temporary mixed ore and herb nodes around a live player anchor.
- Supports dynamic zone selection with blacklist-based restrictions.
- Exposes GM commands for status, force-start, test-start, stop, and reroute.
- Routes random playerbots toward the active hotspot with faction pressure.
- Writes diagnostics to a dedicated `gold-rush.log` logger/appender pair.
- Supports verbose event logging for anchor, spawn, and routing behavior.

## Commands

### GM commands

- `.goldrush status` - show current event state and telemetry.
- `.goldrush start [zone or area]` - start Gold Rush in your current eligible
  zone or in a configured hotspot by name.
- `.goldrush teststart` - force a hotspot at your current eligible location.
- `.goldrush stop` - end the active event and despawn temporary nodes.
- `.goldrush reroute` - refresh playerbot routing pressure.

## Installation

1. Make sure `mod-playerbots` is already installed in your AzerothCore setup.
2. Place `mod-gold-rush` inside the AzerothCore `modules/` directory.
3. Re-run CMake so AzerothCore detects the module.
4. Build and install/update your server.
5. Copy `conf/gold_rush.conf.dist` to your generated module config directory as
   `gold_rush.conf` if you want local overrides.
6. Restart `worldserver`.

## Configuration

The module ships its defaults in `conf/gold_rush.conf.dist`.

Important options:

- `GoldRush.Enable`
- `GoldRush.ZonePool`
- `GoldRush.Blacklist`
- `GoldRush.VerboseLogging`
- `GoldRush.MinIntervalMinutes`
- `GoldRush.MaxIntervalMinutes`
- `GoldRush.DurationMinutes`
- `GoldRush.MinNodes`
- `GoldRush.MaxNodes`
- `GoldRush.BotsPerFaction`
- `GoldRush.BotPulseSeconds`
- `GoldRush.SpawnRadiusYards`
- `GoldRush.NodeEntries`
- `GoldRush.StartMessage`
- `GoldRush.EndMessage`
- `GoldRush.Debug`
- `Appender.GoldRushLog`
- `Logger.module.goldrush`

## Behavior notes

- Event locations are announced approximately, not with exact coordinates.
- Hotspots are temporary and despawn at event end.
- Eligible anchors must be in open-world, non-city, non-sanctuary zones.
- `mod-playerbots` is required because Gold Rush actively routes random bots to
  the event hotspot.
- Blacklisted zones and zone/area pairs are excluded from selection.
- Node selection alternates between ore-like and herb-like entries when
  possible.
- If configured hotspots are unavailable, Gold Rush can fall back to eligible
  live-player zones.

## Repository contents

- `src/` - module implementation and script loader registration.
- `conf/` - distributed module configuration.
- `mod-gold-rush.cmake` - AzerothCore build integration.

## Status

This module is actively being iterated on. Recent work includes:

- looser open-world zone eligibility,
- corrected node entry parsing from config,
- improved test-start behavior and hotspot fallback handling.
