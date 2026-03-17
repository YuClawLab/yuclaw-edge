# Yuclaw-Edge — C++ FIX Execution Gateway

**Sub-millisecond order dispatch via FIX 4.4 protocol.**

Low-latency execution gateway that bridges YUCLAW's AI-generated trade signals to broker FIX endpoints.

## Overview

Yuclaw-Edge handles the last mile of automated trading — receiving structured orders from the YUCLAW engine and dispatching them to brokers via the FIX 4.4 protocol with sub-millisecond latency.

## Key Features

- **Sub-millisecond latency** — kernel-bypass networking with lock-free order queue
- **FIX 4.4 compliant** — full session management (Logon, Heartbeat, Sequence Reset)
- **Risk gates** — pre-trade position limits, notional caps, and rate throttling
- **Replay safe** — deterministic message sequencing with gap-fill recovery
- **Multi-venue** — route to multiple brokers with smart order routing

## Architecture

```
YUCLAW Engine                         Broker
    │                                    │
    ▼                                    ▲
┌────────┐   ┌───────────┐   ┌─────────────┐
│ Order  │──▶│ Risk Gate │──▶│ FIX Session │──▶ TCP/TLS
│ Queue  │   │ (limits)  │   │ (4.4)       │
└────────┘   └───────────┘   └─────────────┘
  lock-free    < 50 μs         < 200 μs
```

## Message Types

| FIX Tag | Message | Direction |
|---------|---------|-----------|
| 35=D    | New Order Single | Outbound |
| 35=F    | Order Cancel Request | Outbound |
| 35=8    | Execution Report | Inbound |
| 35=9    | Order Cancel Reject | Inbound |
| 35=0    | Heartbeat | Both |

## Configuration

```yaml
fix_session:
  sender_comp_id: YUCLAW
  target_comp_id: BROKER
  host: fix.broker.com
  port: 9876
  ssl: true

risk_limits:
  max_order_rate: 100/s
  max_notional: 10000000
  max_position_pct: 0.05
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Part of YUCLAW ATROS

This is a component of the [YUCLAW ATROS](https://github.com/YuClawLab/yuclaw-brain) financial intelligence system.

## License

Proprietary — YuClawLab
