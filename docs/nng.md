# NNG Event System Documentation

This document describes the real-time event system in `urfd`, which uses NNG (nanomsg next gen) to broadcast system state and activity as JSON.

## Architecture Overview

The `urfd` reflector acts as an NNG **Publisher** (PUB). Any number of subscribers (e.g., a middle-tier service or dashboard) can connect as **Subscribers** (SUB) to receive the event stream.

```mermaid
graph TD
    subgraph "urfd Core"
        CR["CReflector"]
        CC["CClients"]
        CU["CUsers"]
        PS["CPacketStream"]
    end

    subgraph "Publishing Layer"
        NP["g_NNGPublisher"]
    end

    subgraph "Network"
        ADDR["tcp://0.0.0.0:5555"]
    end

    subgraph "External"
        MT["Middle Tier / Dashboard"]
    end

    %% Internal Flows
    CC -- "client_connect / client_disconnect" --> NP
    CU -- "hearing / closing" --> NP
    CR -- "periodic state report" --> NP
    PS -- "IsActive status" --> CR

    %% Network Flow
    NP --> ADDR
    ADDR -.-> MT
```

## Messaging Protocols

Events are sent as serialized JSON strings. Every message carries the same three envelope fields, whatever its type:

| Field | Meaning |
|---|---|
| `type` | Identifies the payload structure. |
| `reflector` | Callsign of the reflector that emitted the event. Lets one subscriber watch several reflectors without inferring identity from which socket delivered the message. |
| `timestamp` | UTC time the event was **observed**, `%FT%TZ`. Stamped where the event occurs, not where it is sent, so it stays correct if the event is queued before reaching the socket. |

Note that `callsign`, where a payload carries one, always refers to the station the event is *about* — never to the reflector.

### 1. State Broadcast (`state`)

Sent periodically based on `DashboardInterval` (default 10s). It provides a full snapshot of the reflector's configuration and status.

**Payload Structure:**

```json
{
  "type": "state",
  "reflector": "URF123",
  "timestamp": "2026-09-22T14:03:11Z",
  "Configure": {
    "Key": "Value",
    ...
  },
  "Peers": [
    {
      "Callsign": "XLX123",
      "Modules": "ABC",
      "Protocol": "D-Extra",
      "ConnectTime": "2023-10-27T10:00:00Z"
    }
  ],
  "Clients": [
    {
      "Callsign": "N7TAE",
      "OnModule": "A",
      "Protocol": "DMR",
      "ConnectTime": "2023-10-27T10:05:00Z"
    }
  ],
  "Users": [
    {
      "Callsign": "G4XYZ",
      "Repeater": "GB3NB",
      "OnModule": "B",
      "ViaPeer": "XLX456",
      "LastHeard": "2023-10-27T10:10:00Z"
    }
  ],
  "ActiveTalkers": [
    {
      "Module": "A",
      "Callsign": "N7TAE"
    }
  ]
}
```

### 2. Client Connectivity (`client_connect` / `client_disconnect`)

Triggered immediately when a client (Repeater, Hotspot, or Mobile App) links or unlinks from a module.

**Payload Structure:**

```json
{
  "type": "client_connect",
  "reflector": "URF123",
  "timestamp": "2026-09-22T14:03:11Z",
  "callsign": "N7TAE",
  "ip": "1.2.3.4",
  "protocol": "DMR",
  "module": "A"
}
```

### 3. Voice Activity (`hearing`)

Triggered when the reflector "hears" an active transmission. This event is sent for every "tick" or heartbeat of voice activity processed by the reflector.

**Payload Structure:**

```json
{
  "type": "hearing",
  "reflector": "URF123",
  "timestamp": "2026-09-22T14:03:11Z",
  "callsign": "G4XYZ",
  "repeater": "GB3NB",
  "rpt2": "XLX123 A",
  "via_peer": "XLX123",
  "module": "A",
  "protocol": "M17"
}
```

### 4. Transmission End (`closing`)

Triggered when a transmission stream is closed (user stops talking).

**Payload Structure:**

```json
{
  "type": "closing",
  "reflector": "URF123",
  "timestamp": "2026-09-22T14:03:11Z",
  "callsign": "G4XYZ",
  "module": "A",
  "protocol": "M17"
}
```

## Middle Tier Design Considerations

1. **Late Joining**: The `state` message is broadcast periodically to ensure a middle-tier connecting at any time (or reconnecting) can synchronize its internal state without waiting for new events.
2. **Active Talkers**: The `ActiveTalkers` array in the `state` message identifies who is currently keyed up. Real-time transitions (start/stop) are driven by the `hearing` events and the absence of such events over a timeout (typically 2-3 seconds).
3. **Deduplication**: The `state` report is a snapshot. If the middle-tier is already tracking events, it can use the `state` report to "re-base" its state and clear out stale data.
