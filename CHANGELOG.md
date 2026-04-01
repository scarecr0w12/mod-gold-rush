# Changelog

All notable changes to `mod-gold-rush` will be documented in this file.

## [Unreleased]

### Added
- Standalone repository documentation in `README.md`.
- Initial `CHANGELOG.md` for tracking module releases.
- Clear documentation that `mod-playerbots` is a required dependency.

### Changed
- Loosened Gold Rush hotspot selection so events can fall back to eligible
  live-player zones when configured hotspots are unavailable.
- Improved GM messaging for zone eligibility failures.
- Allowed zone-based fallback when an exact configured sub-area anchor is not
  available.

### Fixed
- Fixed `GoldRush.NodeEntries` parsing so configured node IDs are actually
  loaded from `gold_rush.conf`.
- Fixed startup behavior where Gold Rush could appear to have no valid node
  entries even when configuration values were present.
- Fixed `teststart` and scheduler flows that were overly strict about allowed
  zones.
