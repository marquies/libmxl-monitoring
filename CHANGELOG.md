# Changelog

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
