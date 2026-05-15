<div align="center">

# YUCLAW-EDGE

**FIX 4.4 Reference Client — C++ on ARM64**

![C++](https://img.shields.io/badge/c++-17-blue)
![Protocol](https://img.shields.io/badge/protocol-FIX_4.4-green)
![Hardware](https://img.shields.io/badge/hardware-ARM64_DGX_Spark-purple)
![Status](https://img.shields.io/badge/status-reference_impl-orange)
![License](https://img.shields.io/badge/license-MIT-red)

> Basic FIX 4.4 client in C++ over standard POSIX sockets. Demonstrates wire
> format encoding/parsing and the core message types (Logon, NewOrderSingle,
> OrderCancel, Heartbeat, Logout, ExecutionReport). Compiles on ARM64.

</div>

---

> **Status — reference implementation, not production**
> Production broker dispatch in YUCLAW currently uses the **Python Alpaca
> REST client** in
> [yuclaw-brain](https://github.com/YuClawLab/yuclaw-brain) (`yuclaw/edge/alpaca_gateway.py`).
> This C++ repo is a separate, paper-trading-grade FIX 4.4 protocol
> implementation maintained as a reference for future FIX-broker integrations.
> It does not currently sit in the live signal flow.

---

## What this repo contains

| File | Purpose | LOC |
|---|---|---:|
| `src/fix_gateway.cpp` | FIX 4.4 client (`FIXMessage`, `FIXGateway`, demo `main`) | 461 |
| `CMakeLists.txt` | C++17 build, links pthread | 12 |

Total: **461 lines of C++**, one binary target (`fix_gateway`).

---

## What it actually does

Implemented and working:

- **Wire format encode/parse** — `tag=value` separated by SOH (`\x01`), with
  the leading `8=FIX.4.4|9=<len>|...` envelope and the modulo-256 checksum
  in tag `10`.
- **Session messages** — Logon (`35=A`), Heartbeat (`35=0`), Logout (`35=5`).
- **Order flow** — NewOrderSingle (`35=D`) with Symbol/Side/OrderQty/OrdType/
  TimeInForce/Price, OrderCancelRequest (`35=F`), ExecutionReport (`35=8`)
  parser handling `OrdStatus` 2/4/8 (Filled/Cancelled/Rejected).
- **Latency measurement** — per-order microsecond timestamps via
  `std::chrono::steady_clock`.
- **TCP networking** — standard POSIX `<sys/socket.h>` blocking socket with
  `MSG_DONTWAIT` on the receive path.

Not currently implemented (would be needed for production use):

- Session-recovery messages: Test Request (`35=1`), Sequence Reset (`35=4`),
  Resend Request (`35=2`).
- Multi-venue routing (single sender/target per gateway instance).
- Persistent message store / replay on disconnect.
- TLS — the socket is plain TCP.
- Pre-trade risk gates (those live in `yuclaw-brain`'s Python `RiskGate`
  class, not in this C++ binary).

---

## Build & run

```bash
git clone https://github.com/YuClawLab/yuclaw-edge
cd yuclaw-edge
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
./fix_gateway 127.0.0.1 9876    # connect to a FIX server at host:port
```

Without a FIX server reachable, the binary prints a "simulation mode"
message and exits cleanly — useful for verifying the build but not for
exercising the protocol path.

---

## FIX message types supported

| Tag 35 | Message | Direction | Implemented? |
|:---|:---|:---:|:---:|
| A | Logon | Out | ✓ |
| 0 | Heartbeat | Both | ✓ (basic) |
| 5 | Logout | Out | ✓ |
| D | NewOrderSingle | Out | ✓ |
| F | OrderCancelRequest | Out | ✓ |
| 8 | ExecutionReport | In | ✓ (parse + status update) |
| 9 | OrderCancelReject | In | — |
| 1 | TestRequest | — | — |
| 4 | SequenceReset | — | — |

---

## Architecture note

```
yuclaw-brain (Python)  ─── current production path
   └── yuclaw/edge/alpaca_gateway.py  →  Alpaca REST  →  broker

yuclaw-edge (C++)  ─── this repo, reference FIX implementation
   └── fix_gateway.cpp                →  TCP/FIX 4.4 →  (broker, when wired up)
```

When YUCLAW grows beyond Alpaca-only paper trading, this repo is the
foundation that the FIX-protocol broker integrations will be built on.

---

## Ecosystem

| | |
|:---|:---|
| Production pipeline | [yuclaw-brain](https://github.com/YuClawLab/yuclaw-brain) |
| Live dashboard | [yuclawlab.github.io/yuclaw-brain](https://yuclawlab.github.io/yuclaw-brain) |
| PyPI | [pypi.org/project/yuclaw](https://pypi.org/project/yuclaw) |
| GitHub | [YuClawLab](https://github.com/YuClawLab) |

---

## Disclaimer

YUCLAW is open-source research and educational software. **It is NOT
financial advice or a recommendation to buy or sell any security.** This
repository is a protocol-level reference implementation and is not currently
used in any production trading flow. Trading involves substantial risk of
loss. You are solely responsible for your own investment decisions.

For educational and research purposes only. MIT Licensed.

---

<div align="center">

*Built on NVIDIA DGX Spark GB10 · ARM64 · MIT*

</div>
