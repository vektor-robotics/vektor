# v1 compatibility contract

VEKTOR v1 treats the interfaces below as stable. A compatible v1 release may
add optional protobuf fields, optional configuration fields, and new JSON
fields, but it will not reuse protobuf field numbers, rename or remove existing
RPCs, require a new configuration field, change an existing CLI JSON field's
meaning, or require a newer runtime-driver interface from an existing v1
driver. Breaking changes require a new major version and an explicit migration
guide.

| Surface | Stable contract | Supported input | Current output |
| --- | --- | --- | --- |
| Agent gRPC API | `vektor.agent.v1.Agent`; field numbers and enum values are never reused | API v1 | `StatusSnapshot` schema 1; `DeploymentRecord` schema 5 |
| Health-check YAML | Optional `schema_version: 1`; unversioned legacy files remain valid | unversioned, 1 | n/a |
| Fleet YAML | Optional `schema_version: 1`; unversioned legacy files remain valid | unversioned, 1 | fleet JSON schema 1 |
| Rollout YAML/state | rollout config schema 1; persisted rollout state schemas 1 and 2 | config 1; state 1, 2 | rollout state 2; JSON schema 3 |
| Deployment state | persisted agent schemas 1 through 5 are migrated on read | 1, 2, 3, 4, 5 | schema 5 |
| Authorization policy | schema 1 roles and schema 2 scoped grants | 1, 2 | authorization error JSON schema 1 |
| Trust and approval YAML | explicit schema 1 | 1 | n/a |
| Runtime, verifier, audit interfaces | `interface_version() == 1` | 1 | 1 |

All machine-readable CLI output includes a `schema_version`. Consumers must
ignore unknown JSON fields and reject an unsupported schema version. Operators
should preserve deployment state before upgrades and follow the
[upgrade/recovery runbook](upgrade-recovery.md) for downgrade or recovery.

The protobuf descriptor and configuration compatibility behavior are covered by
the regular ROS 2 Jazzy test suite. Any change to a listed surface requires a
compatibility note in the pull request and an accompanying migration test.
