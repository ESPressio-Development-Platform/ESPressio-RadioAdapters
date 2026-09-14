# ESPressio RadioAdapters

`ESPressio-RadioAdapters` is the direct composition layer between `ESPressio-Radio` and the Primitive families.

It is intentionally the **only** layer that understands both Radio-specific physical transport concepts and Primitive family identifiers/policies. `ESPressio-Radio` remains Primitive-family-neutral; generic `ESPressio-Adapters` remains Radio-neutral.

## Ownership boundary

```text
Event / Command / State
        |
        v
  family Radio binding
        |
        v
ESPressio-RadioAdapters
   |              |
   v              v
Adapters (A2)   Radio
```

RadioAdapters owns:

- the exact direct-Radio four-byte family/version prefix;
- explicit neutral Adapter service ↔ Radio service mapping;
- fixed/frozen direct-Radio family-policy bindings;
- trusted physical/semantic provenance mapping at ingress;
- bounded Radio lower-transport submission into A2;
- exact M1 destination-admission receipt correlation;
- generation-safe restart/quiesce handling for transport-owned correlation;
- frozen Event, Command and State direct-Radio family bindings.

It does **not** own:

- Event/Command/State family semantics or wire representations;
- Radio physical fragmentation/reassembly;
- a second family runtime;
- a family-local retry queue or worker;
- application execution.

## Exact direct-Radio envelope

The prefix is locked and wire-stable:

```text
bytes 0..1  PrimitiveFamilyId        little-endian
bytes 2..3  PrimitiveProtocolVersion little-endian
bytes 4..N  unchanged family representation
```

No payload-length field, semantic provenance field or transport-specific family wrapper is added by RadioAdapters. Radio service class remains out-of-band and is mapped explicitly.

Use the public umbrella:

```cpp
#include <ESPressio_RadioAdapters.hpp>
```

## Inbound path

The canonical inbound path is:

```text
Radio completes one owned logical message
-> RadioAdapters validates four-byte family/version prefix
-> trusted Radio provenance/service is mapped to neutral Adapter metadata
-> A2 owns/copies the family bytes
-> one frozen family admission thunk executes
-> Event/Command/State returns exact Primitive admission disposition
```

Malformed prefixes, unsupported families/protocols and invalid service/policy combinations fail before arbitrary family delivery.

An immediate physical Radio peer is **not** automatically the authenticated semantic `OriginalSource`. Where a composition can validate semantic source provenance, RadioAdapters preserves that as a separate fact and family bindings such as Command/State cross-check it against the family representation before mutation/execution.

## Outbound path

A family binding exposes its immutable family representation directly to A2. `RadioAdapterLowerTransport` prepends the four-byte prefix in one fixed workspace and submits the logical message through the Radio runtime.

The lower transport has no queue of its own. Workspace contention returns bounded temporary unavailability; Radio owns physical scheduling and fragmentation.

## Exact M1 destination admission

Radio link completion is not destination Primitive admission.

When policy requires `DestinationPrimitiveAdmission`, `RadioAdapterM1Controller` reserves one bounded correlation attempt and the destination sends an exact admission receipt. The seven dispositions are:

```text
Accepted
AlreadyAccepted
TemporarilyUnavailable
ResourceUnavailable
Unsupported
Rejected
Malformed
```

Only `Accepted` and `AlreadyAccepted` establish destination admission evidence.

A successful Radio transmission, including direct-link peer acknowledgement, can never substitute for that receipt.

## Lifecycle / bounded correlation

Transport-owned state is finite:

- `RadioAdapterLowerTransport<TRadioRuntime, TMaximumLogicalMessageBytes>` owns exactly one fixed synchronous workspace;
- `RadioAdapterM1Controller<TRadioRuntime, TMaximumAttempts, TRecentTransferIds>` owns fixed active-attempt and recent-transfer-ID arrays;
- `RadioAdapterReassemblyIngress<..., TMaximumPendingReceipts>` owns fixed pending receipt slots;
- lifecycle generations are non-wrapping/fail-closed;
- quiesce stops new admission and releases volatile correlation;
- old lifecycle callbacks/receipts/completions cannot become current work after restart;
- no hidden retry worker or unbounded queue is introduced.

## Event binding

`EventRadioAdapterFamilyBinding<TMaximumTypes>` freezes per-Event-Type:

- canonical Event Type ID;
- unchanged Event wire codec/maximum bytes;
- immutable Primitive delivery policy;
- explicit Adapter service class;
- Event runtime admission binding.

`EventRadioAdapterOutboundTarget` routes local transmissible occurrences into A2. Remote-origin Event handling remains owned by the Event runtime; RadioAdapters does not create a second Event dispatch engine.

## Command binding

`CommandRadioAdapterFamilyBinding` maps semantic destination `System::DeviceIdentifier` values through `RadioAdapterSemanticRouteBinding` to opaque Adapter route tokens. Semantic identity is never packed or hashed into the transport route token.

Command remains the owner of handler cardinality, request/response semantics, idempotency, durable recovery and execution. Response-bearing Commands retain only the bounded Command delivery token/correlation required by A2; RadioAdapters never becomes a Command executor or ledger.

## State binding

`StateRadioAdapterFamilyBinding` carries canonical State v1 publications/control representations through A2 and direct Radio while State remains the owner of authoritative values, sessions, versions, resynchronization and `NeedsConvergence` semantics.

Terminal lower-transport exhaustion is fed back through the State runtime rather than creating a RadioAdapter retry engine.

## Dependency direction

The conceptual direct dependencies are the locked DAG:

```text
Radio
Adapters
Primitive
Event
Command
State
```

RadioAdapters currently has no `library.json` or `library.properties`; the redesign does not invent a package manifest merely for symmetry.

## Validation

The redesign branch has dedicated permanent contracts for:

- exact envelope vectors and service mapping;
- ingress provenance and malformed-prefix rejection;
- lower-transport bounded admission;
- all seven exact-M1 dispositions;
- lifecycle/restart/stale-completion handling;
- Event direct-Radio policy/round-trip behavior;
- Command direct-Radio request/response and recovered-response behavior;
- State direct-Radio outbound/inbound/full-ingress behavior;
- adversarial wrong route/domain/transfer IDs and direct-link-ACK non-establishment.

All current redesign workflows use the matching `primitives_redesign` dependency branches.
