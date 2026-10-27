# Changelog

## [0.3.0](https://github.com/marquies/libmxl-monitoring/compare/v0.2.0...v0.3.0) (2026-10-27)


### Features

* record what a writer's libmxl calls return ([#9](https://github.com/marquies/libmxl-monitoring/issues/9)) ([16b52ec](https://github.com/marquies/libmxl-monitoring/commit/16b52ecd0df0556c5a1dfeda4aabe95ea2578287))
* serve /metrics from a process without an HTTP server ([#10](https://github.com/marquies/libmxl-monitoring/issues/10)) ([c084354](https://github.com/marquies/libmxl-monitoring/commit/c08435496612ab409a19192bcb35f98afc1e33ff))


### Bug Fixes

* count a domain that went away as unavailable ([#8](https://github.com/marquies/libmxl-monitoring/issues/8)) ([ae95acc](https://github.com/marquies/libmxl-monitoring/commit/ae95acc96995d76b6dac994a3332c69013edf3b1))


### Continuous Integration

* test against the libmxl of go-mxl 1.1.0-rc.7 ([#6](https://github.com/marquies/libmxl-monitoring/issues/6)) ([af33312](https://github.com/marquies/libmxl-monitoring/commit/af333124d90b7b46b42100c04946881fadb7b667))

## [0.2.0](https://github.com/marquies/libmxl-monitoring/compare/v0.1.0...v0.2.0) (2026-10-25)


### ⚠ BREAKING CHANGES

* mxlMonitoringFlowSnapshot and mxlMonitoringEvent carry the flow id in MXL_MONITORING_ID bytes, the snapshot gains the target, and MXL_MONITORING_ACCESS_DISCONNECTED precedes MXL_MONITORING_ACCESS_COUNT.

### Features

* report a flow handle under a name of its own ([#3](https://github.com/marquies/libmxl-monitoring/issues/3)) ([87d843f](https://github.com/marquies/libmxl-monitoring/commit/87d843fb4db26a639e05b7fda5fe17e092020de0))

## 0.1.0 (2026-10-24)


### Features

* record MXL flow outcomes and expose them for Prometheus ([#1](https://github.com/marquies/libmxl-monitoring/issues/1)) ([665bb22](https://github.com/marquies/libmxl-monitoring/commit/665bb220df7db29603c6e9bd6db18a13c9036040))


### Miscellaneous

* add the Apache-2.0 license ([2f996b5](https://github.com/marquies/libmxl-monitoring/commit/2f996b57f414282f633b611f778125668bc74c7a))
