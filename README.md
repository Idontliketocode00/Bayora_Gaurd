# Bayora-Guard (C++ prototype)
Team Legally 401: Hack in Hills

Red and blue teams test an LLM without ever touching each other. Everything flows through a **blind gateway**
that gives every request the same response shape and the same response time and records every decision in a **SHA-256 hash-chained ledger**.

```
 red-net            blue-net (internal)       llm-net (internal)
[ red ] <--> [        GATEWAY        ] <--> [ blue ]
                 [ (only node on all 3) ] <--> [ llm (Ollama / TinyLlama) ]
```

Everything is plain C++17 with **no third-party libraries**: SHA-256, JSON, HTTP server/client are all in `include/`.
The only non-C++ part is Ollama (the LLM itself).

## How the timing defence works (fixed deadline)
Every request is answered at **exactly `DELAY_BUCKET_SECONDS`** after it arrived (default 3 s with the mock LLM,
15 s with a real LLM), whether it was allowed, blocked or failed. The pipeline gets that time as a budget: if the LLM
cannot answer in time, the request **fails closed at the same moment** and is logged as `error`. So a slow answer can
never be told apart from a blocked one by latency. (`FIXED_DEADLINE=0` restores the older mode that rounds up to
the next multiple of the bucket; it leaks, because slow allowed requests take longer than blocked ones.)

With a real model, give it enough time and keep replies short:
`DELAY_BUCKET_SECONDS=20 LLM_NUM_PREDICT=40`, and warm the model first (see below).

## Quick start without Docker (fastest; uses the mock LLM)
```bash
make            # builds bin/blue bin/gateway bin/red bin/selftest
make test       # 25 checks: NIST SHA-256 vectors, JSON, ledger tamper detection, guardrail rules

# three terminals, from this folder:
PORT=8001 ./bin/blue
BLUE_HOST=127.0.0.1 MOCK_LLM=1 ./bin/gateway
GATEWAY_HOST=127.0.0.1 ./bin/red
```
Real LLM (Ollama installed on the machine; warm it first so it is loaded):
```bash
curl -s localhost:11434/api/generate -d '{"model":"tinyllama","prompt":"hi","stream":false,"options":{"num_predict":5},"keep_alive":"1h"}'
STATIC_DIR=gateway/static BLUE_HOST=127.0.0.1 LLM_HOST=127.0.0.1 DELAY_BUCKET_SECONDS=20 LLM_NUM_PREDICT=40 ./bin/gateway
```
Open http://localhost:8000 for the dashboard (auditor token: `bayora-demo`). Run the gateway from this folder, or set `STATIC_DIR=gateway/static`.

## Full run with Docker (isolated networks + real TinyLlama)
```bash
docker compose up --build     # first build downloads TinyLlama (~640 MB)
```
- `red` prints an **isolation check** (gateway reachable; blue and LLM not), then runs 8 prompts and prints timings.
- Dashboard at http://localhost:8000. No model download? Set `MOCK_LLM: "1"` in `docker-compose.yml`.
- Prove isolation yourself: `docker exec bayora-red getent hosts blue` returns nothing.

## 2-minute demo script
1. Show `docker compose up`: the red container's **isolation check** and the 8-row table (all about 3.00 s, same JSON keys).
2. Dashboard: send the *benign* chip, then *injection*. Both take 3.0 s and look identical to the red team.
3. Ledger panel (auditor view): benign = `allowed`, injection = `blocked [LLM01 Prompt Injection]`.
4. Click **Tamper with entry #0**, then **Verify chain**: it turns red at the exact entry.

## Read the code in this order (learning path)
| # | File | What to learn | Try this |
|---|------|---------------|----------|
| 1 | `include/sha256.hpp` | padding, message schedule, 64 rounds, `uint32_t` wraparound, `rotr` | Change `K[0]` by one bit; watch `make test` fail |
| 2 | `include/ledger.hpp` | hash chain, `std::mutex`, JSON-lines persistence, length-prefixed canonical form | Edit `ledger.jsonl` by hand; run `/verify` |
| 3 | `include/rules.hpp` | `std::regex`, normalisation | Add a rule; find a prompt that bypasses the rules |
| 4 | `include/json.hpp` | escaping, UTF-8, a hand-written parser | Break `json_escape` and see which test catches it |
| 5 | `include/net.hpp` | sockets, HTTP framing, chunked decoding, timeouts, thread per connection | Run `curl -v` against the blue service |
| 6 | `blue/main.cpp`, `red/main.cpp` | small services built on the above | |
| 7 | `gateway/main.cpp` | the core: pipeline, fail-closed, fixed deadline with `sleep_until` | Run with `FIXED_DEADLINE=0` and a slow LLM, and watch the timing leak appear |

Key idea to be able to explain in the viva: the gateway replies at a *fixed point in time* (`start + bucket`)
and gives blue and the LLM only the time left in that budget, so total latency does not depend on how long they took.

## Endpoints
| Service | Endpoint | Notes |
|---|---|---|
| Gateway :8000 | `POST /submit` `{"prompt":"..."}` | Public to red; identical reply shape for allowed/blocked/error |
| Gateway | `GET /ledger`, `GET /verify` | Auditor only: header `X-Auditor-Token` |
| Gateway | `POST /demo/tamper/{i}`, `POST /demo/reset` | Only when `DEMO_MODE=1` |
| Blue :8001 | `POST /inspect` | Reachable only from the gateway |
| LLM :11434 | Ollama `/api/generate` | Reachable only from the gateway |

Configuration is by environment variables (see `docker-compose.yml` and the `Config` struct in `gateway/main.cpp`). `LLM_NUM_PREDICT` (default 100) caps the reply length in tokens; `DELAY_BUCKET_SECONDS` is the fixed reply time; `FIXED_DEADLINE=0` switches to the old rounded-bucket mode (then `LLM_TIMEOUT_SECONDS` applies).

## Honest limitations
- The guardrail is rule-based regex (easy to bypass; no Unicode look-alike folding). The architecture is the contribution.
- A fixed deadline removes latency as a signal between allowed, blocked and failed requests, but it does not prove that every side channel is gone (the cost is that every reply waits the full deadline).
- The ledger is tamper-*evident*, not tamper-proof: someone who can rewrite the whole file can rebuild the chain
  unless the head hash is anchored somewhere external.
- Auditor access uses a shared demo token. Production needs real authentication and a separate audit network.
- The HTTP server is minimal (one thread per connection, no TLS, no keep-alive). Fine for a prototype, not for production.
- Future scope (not built): Kubernetes/Kata Containers, SIEM integration, ML-based blue team, TinyFish agents.
