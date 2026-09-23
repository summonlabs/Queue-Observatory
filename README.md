# Queue Observatory

**Vendor-neutral observation and historical interpretation of network queue state.**

Queue Observatory is a C++20 runtime from Summon Software Labs that ingests
observations of network queues, decides what those observations are allowed to
mean, keeps a bounded and explainable history, and can persist and recover that
history without ever promoting old evidence into a current claim.

It observes. It does not act.

---

## The boundary, stated precisely

Queue Observatory **owns**:

* observation of queue state,
* historical interpretation of that state.

Queue Observatory **does not**:

* schedule queues, shape traffic, or reorder anything,
* change QoS, DSCP, PCP, or any classification,
* allocate, reserve, or free buffers,
* run, tune, or influence congestion control,
* program hardware, write registers, or push configuration,
* speak any device-management protocol.

There is no code path in this repository that can change a device. The public
API accepts evidence and answers questions about it; it exposes no setter that
reaches a device, because no such setter exists. The ingest endpoint accepts
observation documents and nothing else. This is a structural property of the
code, not a policy that a configuration flag could turn off.

---

## Proof surfaces

Every claim in this repository is labelled by what actually backs it.

### REAL

These are exercised by the test suite on this machine, in this repository.

| Surface | How it is proven |
| --- | --- |
| Observation decode and validation | Unit, property and adversarial suites over the canonical format |
| Counter continuity, wrap and reset interpretation | Unit suite plus a seeded randomized invariant suite |
| Deterministic idle/normal/elevated/pressured/saturated/dropping/paused/stale/unknown/conflicting states | Every state is asserted reachable, with the deciding rule and the full trace |
| Deterministic explanations | Repeated classification of identical input is compared byte for byte, including a digest over the canonical explanation |
| Bounded occupancy and event history | Ring capacity, eviction accounting and byte budgets are asserted directly |
| Microburst evidence | Peak-to-trough evidence over observed buckets only, with explicit insufficient-coverage reporting |
| Sibling contention correlation | Exact-rational correlation over buckets in which both siblings were observed |
| Class metadata correlation | Metadata revisions are versioned, fenced and reported as the origin of the class attributes they supply |
| Persistence and conservative recovery | Round-trip, header damage, payload damage, truncation, rotation and fallback are all tested |
| Restart semantics | Persisted evidence is re-admitted as stale and provably cannot define a current state until fresh evidence arrives |
| Concurrency and shutdown | Multi-threaded submit/read/stop tests, with the lock-rank audit asserted at zero violations |
| Real inter-process transport | A server process started by the test and a client process started by the test exchange canonical documents over loopback TCP |
| Installable package | An independent CMake project outside the build tree consumes the installed package through find_package |

### SYNTHETIC

These are real code paths fed with constructed inputs. No hardware was involved.

| Surface | What is synthetic about it |
| --- | --- |
| Every observation used in the tests, examples and benchmarks | The documents are generated in-process from the canonical encoder; no switch, NIC or agent produced them |
| The transport tests | The two processes are real and the TCP connection is real, but both ends are this runtime |
| The metadata tables | Traffic-class and scheduling-class tables are authored by the tests, not read from a device |

### UNSUPPORTED

These are explicitly **not** implemented, and no claim is made about them.

| Surface | Status |
| --- | --- |
| Any switch, ASIC, NPU or SmartNIC integration | Not implemented. No vendor SDK, no register access, no gNMI/NETCONF/SNMP/INT collector |
| RDMA, RoCE, InfiniBand, NVLink, or any fabric-specific telemetry | Not implemented and not modelled |
| Multi-host or fabric-wide correlated observation | Not implemented. Correlation is limited to queues that share a device and port inside one runtime |
| Sub-sample reconstruction between observations | Refused by design. The runtime never invents behaviour between two samples; a burst is reported only from buckets that were actually observed, and under-coverage is reported as under-coverage |
| In-band telemetry, P4, eBPF, or hardware timestamps | Not implemented |
| Time synchronization across clock domains | Not attempted. Timestamps from different clock domains are never compared; the comparison is refused and reported |
| Authentication, authorization, or encryption of the transport | Not implemented. The transport is a plain TCP framing for loopback or an explicitly configured endpoint |
| Backpressure signalling towards a device | Not implemented and out of scope: the runtime only reads |

---

## Building

Requirements: CMake 3.20 or newer and a C++20 compiler. MSVC 19.30+ is the
configuration this repository is validated on; the code is also written to build
warn-free with GCC and Clang using the equivalent strict flags.

    cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64
    cmake --build build/release --config Release
    ctest --test-dir build/release -C Release --output-on-failure

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `QOBS_BUILD_TESTS` | `ON` | Build the seven test executables and register them with CTest |
| `QOBS_BUILD_EXAMPLES` | `ON` | Build the three example programs |
| `QOBS_BUILD_BENCHMARKS` | `ON` | Build the benchmark executables |
| `QOBS_BUILD_TOOLS` | `ON` | Build the `qobs` CLI, the transport server and the transport client |
| `QOBS_WARNINGS_AS_ERRORS` | `ON` | Treat first-party warnings as errors |
| `QOBS_ENABLE_ASAN` | `OFF` | Build with AddressSanitizer (MSVC `/fsanitize=address`, GCC/Clang `-fsanitize=address`) |
| `QOBS_ENABLE_LOCK_AUDIT` | `ON` | Compile the lock-rank and reentrancy audit instrumentation |

Additional validated configurations:

    # AddressSanitizer (MSVC needs debug information; the runtime DLL must be on PATH)
    cmake -S . -B build/asan -G "Visual Studio 17 2022" -A x64 -DQOBS_ENABLE_ASAN=ON
    cmake --build build/asan --config Release
    ctest --test-dir build/asan -C Release --output-on-failure

    # Debug
    cmake -S . -B build/debug -G "Visual Studio 17 2022" -A x64
    cmake --build build/debug --config Debug
    ctest --test-dir build/debug -C Debug --output-on-failure

Both configurations build with zero first-party warnings and both run the whole
suite green.

---

## Quick start

Write a canonical observation document. It is newline-delimited JSON; blank
lines and lines starting with `#` are ignored.

    {"kind":"queue_sample","v":1,"device":"leaf-01","port":"ethernet1/1","queue":3,
     "source":"telemetry-a","incarnation":"boot-7","generation":12,"sequence":1001,
     "authority":"primary","observed_ns":1700000000000000000,
     "clock_domain":"device-utc","occupancy_cells":820,
     "dynamic_threshold_cells":1000,"enqueue_packets":91000,"drop_packets":0}

(The real document is one line; it is wrapped here for readability.)

    qobs ingest --file observations.ndjson
    qobs inspect
    qobs pressure --state dropping
    qobs history --queue-path 'leaf-01|ethernet1/1|3' --window-ns 1000000000 --bucket-ns 125000000
    qobs explain --queue-path 'leaf-01|ethernet1/1|3'
    qobs export --format ndjson --history --window-ns 1000000000 --bucket-ns 125000000

Options may appear before or after the command. `--persist-dir` enables
persistence and recovery; a second run over the same directory recovers the
previous session and reports the recovered evidence as stale.

The canonical text form of a queue path is `device|port|queue`. The pipe is the
separator precisely so that interface names containing a slash, such as
`Ethernet1/1`, need no escaping.

---

## Using the library

    #include <qobs/runtime/Observatory.hpp>
    #include <qobs/ingest/Wire.hpp>

    qobs::ObservatoryConfig config;
    config.runtime.worker_threads = 0;           // 0 runs synchronously
    auto created = qobs::Observatory::create(config, nullptr);
    std::unique_ptr<qobs::Observatory> runtime = std::move(created).value();
    runtime->start();

    qobs::IngestReport report;
    runtime->ingest_document(document, report);
    if (report.admission.rejected != 0) { /* the report says why */ }

    qobs::PressureQuery query;
    qobs::PressureResult pressure;
    runtime->pressure(query, pressure);

    runtime->stop();

Everything returns a result or a status value. Nothing throws.

### Installed package

    cmake --install build/release --config Release --prefix /some/prefix
    cmake -S tests/downstream -B build/consumer -DCMAKE_PREFIX_PATH=/some/prefix
    cmake --build build/consumer --config Release
    ./build/consumer/qobs-downstream-consumer

The downstream project is deliberately outside the build tree and links only
the exported targets `QueueObservatory::Runtime`, `QueueObservatory::Ingest`,
`QueueObservatory::Persist` and `QueueObservatory::Transport`.

---

## Architecture

    qobs_core        identities, results, checked arithmetic, CRC-32C, time,
                     JSON, text, limits, lock audit, cancellation
    qobs_policy      thresholds, the ordered rule set, decision traces,
                     deterministic explanations
    qobs_store       bounded history rings, window aggregation, sibling
                     contention, microburst evidence, the evidence store
    qobs_persist     versioned, integrity-checked segments with conservative
                     recovery
    qobs_ingest      the canonical wire format: strict decode, strict encode
    qobs_runtime     the Observatory facade, the ordered applier, queries,
                     reports, snapshot and recovery
    qobs_transport   TCP framing, the ingest server and the ingest client

Each layer is a separate static library with a public header set under
`include/qobs/`. The dependency direction is one way: transport depends on
runtime, runtime on persist, ingest, store and policy, and everything on core.

### Identities

Device, port, queue, traffic class, scheduling class, source, incarnation,
generation, source sequence and revision are distinct types. A queue index
cannot be passed where a port index is expected, because the compiler will not
accept it.

Two decisions are worth calling out:

* **Traffic class and scheduling class are not part of a queue's identity.**
  They are attributes that may be observed in a sample, resolved from class
  metadata, or simply unknown, and the runtime records which of those happened.
* **Queue index zero is an ordinary queue.** Validity is decided by the device
  and port, never by the index.

### Evidence

Every sample records what was observed, by which source, for which generation,
at what observation and receive times, under which source incarnation, and
whether the evidence is fresh, stale, conflicting, incomplete, unsupported or
unknown.

* **Missing is not zero.** A measurement that was not reported is absent. The
  fields a sample claims to report travel with it, and the classifier refuses to
  use a measurement whose reported bit is not set.
* **Receive time is a fact about this runtime.** The canonical format carries no
  receive timestamp; the runtime stamps one when evidence arrives. Freshness is
  always computed from that monotonic clock, never from a sender's claim.
* **Clock domains are never crossed.** A timestamp from one domain is never
  compared with a timestamp from another; the comparison is refused and the
  refusal is recorded.
* **Absence of evidence is never positive evidence.** Only fresh, complete,
  non-recovered evidence from a source with at least primary authority may
  define a queue's current state.

### Deterministic states

The classifier evaluates thirteen rules in a fixed order and stops at the first
match:

| Order | Rule | Produces |
| --- | --- | --- |
| 0 | evidence presence | `unknown` |
| 1 | evidence conflict | `conflicting` |
| 2 | evidence unsupported | `unknown` |
| 3 | evidence freshness | `stale` |
| 4 | required fields | `unknown` |
| 5 | pressure basis | `unknown` |
| 6 | dropping evidence | `dropping` |
| 7 | pause evidence | `paused` |
| 8 | saturation level | `saturated` |
| 9 | pressure level | `pressured` |
| 10 | elevation level | `elevated` |
| 11 | idle evidence | `idle` |
| 12 | default | `normal` |

Thresholds are either per-mille of a reported limit (integer arithmetic, rounded
up) or absolute values from the policy. No floating point value takes part in
any decision.

Every classification returns a trace of exactly the rules that were reached, and
a digest over the canonical explanation. Two runs of the same evidence under the
same policy produce identical states, identical traces and identical digests.

### Persistence and recovery

A segment is a fixed header, an optional runtime identifier and a sequence of
records. The header carries the format version, the policy version, the writer's
version, the record count, the payload size and a CRC-32C over both the header
and the payload. Each record carries its own CRC-32C over its prefix and value.

Recovery is conservative:

* a segment whose magic, header size, header checksum or version is wrong is
  refused entirely,
* a segment that is truncated or has a damaged record yields the records that
  were fully readable, together with a report that says exactly what happened,
* recovered samples are re-admitted as history with the recovered provenance and
  a capped freshness, so a restart can never silently promote old evidence into a
  current state; only new, fresh evidence can do that.

Writes are atomic: a temporary file is written, flushed and synced, then renamed
over the segment name. A crash leaves either the previous segment or the new
one, never a half-written file a reader could mistake for complete.

### Concurrency and lock discipline

See `docs/CONCURRENCY.md` for the full audit. In short: a small fixed set of
lock ranks, acquisition in strictly increasing rank order, no same-rank nesting,
and no callback ever invoked while a lock is held. The audit is compiled in and
its violation counters are asserted zero by the test suite, and a deliberately
provoked-violation test proves the detector itself works.

Background workers decode submitted documents in parallel and apply them in
submission order, so the observable result does not depend on the number of
workers. Shutdown requests cancellation, wakes every worker, joins every thread
and discards queued work with an explicit count.

### Bounded everything

Every unbounded input has a named limit that is validated when a component is
constructed: payload size, record count per line, JSON depth and node count,
field length, samples per batch, queues, sources, history records per queue,
total history bytes, events, conflicts, aggregation buckets, persistence segment
size and count, pending batches, worker count and transport frame size. Sizes
derived from external input go through checked arithmetic; overflow is reported,
never wrapped.

---

## Tooling

| Command | Purpose |
| --- | --- |
| `qobs ingest` | Decode and apply observation documents |
| `qobs inspect` | List tracked queues with their current state |
| `qobs history` | Bounded history records plus an aggregated window of one queue |
| `qobs pressure` | Deterministic pressure classification, with `--trace` for the full decision path |
| `qobs explain` | The complete decision trace of one queue |
| `qobs export` | Deterministic NDJSON or text export, optionally including window buckets |
| `qobs status` | Runtime counters, queue counts and persistence statistics |
| `qobs recover` | Re-read the newest persistence segment and report what was usable |
| `qobs events` | The bounded event log, filterable by kind and severity |
| `qobs sources` | The source registry and its fencing counters |
| `qobs conflicts` | Recorded peer disagreements |
| `qobs burst` | Observed microburst evidence |
| `qobs contend` | Sibling contention correlation |
| `qobs version` | Build and format versions |

`qobs-transport-server` and `qobs-transport-client` carry canonical documents
over TCP. The server accepts observations and answers with what the runtime
actually did; it has no configuration path.

---

## Testing

Seven suites, all registered with CTest. **No test uses a timeout.** A test
either reaches its conclusion or the process is killed by the operating system,
which is what makes a hang a real defect rather than a flake.

| Suite | Contents |
| --- | --- |
| `qobs_unit_tests` | Identities, checked arithmetic, CRC, JSON, time, lock audit, counters, evidence, policy, classification, windows, the store, persistence, history |
| `qobs_property_tests` | Seeded randomized invariants: counter totals never invent progress, classification is a total and reproducible function, non-fresh evidence never yields a live state, window aggregates respect their own bounds, the wire round trip preserves every reported field |
| `qobs_integration_tests` | Document decode and apply, runtime lifecycle, ordered asynchronous application, facades, full end-to-end paths including restart |
| `qobs_adversarial_tests` | Hostile JSON, absurd nesting, embedded NUL bytes, replay storms, queue floods, clock regression, malformed frames |
| `qobs_concurrency_tests` | Many submitters, concurrent readers during writes, cancellation, parallel store queries, concurrent metadata revisions |
| `qobs_recovery_tests` | Recovered evidence is stale and incomplete, event replay, truncated segments, damaged stores, restart cycles |
| `qobs_transport_tests` | Loopback round trip, plus a server process and a client process started by the test |

The transport suite starts real operating-system processes. The listening socket
is created by the test and inherited by the server process, so the test never
waits for a port announcement and never guesses when the server is ready. The
server process exits after a batch budget it was given, so termination is
deterministic rather than timeout-driven.

---

## Benchmarks

Benchmarks report completed work: the count of operations that ran to completion
alongside the elapsed time. Nothing measures a projection.

Measured on this machine, Release, MSVC 19.44, single run:

    decode_document (samples)          completed=20000      0.092s  218039 sample/s
    ingest_batch (samples accepted)    completed=20000      0.091s  220258 sample/s
    classify (decisions)               completed=200000     0.872s  229366 decision/s
    window observations folded         completed=200000     0.001s  162879713 observation/s
    persistence encode+decode          completed=200      0.025s  8050 round/s
    end-to-end documents applied       completed=64         0.084s  758 document/s   (1 worker)
    end-to-end documents applied       completed=64         0.025s  2603 document/s  (4 workers)
    transport round trips              completed=200       0.105s  1910 round-trip/s

These numbers are a record of one run on one machine. They are not a promise.

---

## Limitations

* Correlation, contention and microburst interpretation are limited to queues
  that share a device and port inside one runtime. There is no fabric-wide view.
* The transport is plain TCP with no authentication, authorization or
  encryption. It is intended for loopback or an explicitly trusted endpoint.
* Time synchronization between a device clock and the host clock is not
  attempted. Observation times from different clock domains are recorded but
  never compared.
* Class metadata must be supplied by a caller. No metadata is discovered.
* AddressSanitizer is validated on MSVC only on this machine. When a toolchain
  does not support it, `build_has_address_sanitizer()` reports false and no
  equivalent coverage is claimed.
* Memory coverage under ASan is not the same as coverage under a race detector.
  ThreadSanitizer was not run, and no equivalent claim is made.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
