# Validation record

This is the procedure that was run against this repository, and what it
observed. Every command below was executed from the repository root.

## 1. Release configuration

    cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64 -DQOBS_WARNINGS_AS_ERRORS=ON
    cmake --build build/release --config Release --parallel

Result: builds clean. First-party warning count is zero; warnings are errors.

## 2. Test suite, Release

    ctest --test-dir build/release -C Release --output-on-failure

Result: 7 of 7 suites pass.

## 3. Debug configuration

    cmake -S . -B build/debug -G "Visual Studio 17 2022" -A x64
    cmake --build build/debug --config Debug --parallel
    ctest --test-dir build/debug -C Debug --output-on-failure

Result: builds clean; 7 of 7 suites pass.

## 4. AddressSanitizer

    cmake -S . -B build/asan -G "Visual Studio 17 2022" -A x64 -DQOBS_ENABLE_ASAN=ON
    cmake --build build/asan --config Release --parallel
    set PATH=<MSVC bin>\Hostx64\x64;%PATH%
    set ASAN_OPTIONS=detect_leaks=0
    ctest --test-dir build/asan -C Release --output-on-failure

Result: builds clean; 7 of 7 suites pass with the sanitizer active.

ASan found one defect during hardening: an assertion helper bound a reference to
a member of a temporary result object, which dangled for the duration of the
comparison. The assertion helpers were rebuilt as functions so that every
temporary lives for the whole comparison. Leak detection is disabled because the
runtime intentionally keeps process-lifetime singletons; memory-error detection
is fully active.

## 5. Install and downstream consumption

    cmake --install build/release --config Release --prefix build/install
    cmake -S tests/downstream -B build/consumer -G "Visual Studio 17 2022" -A x64 \
          -DCMAKE_PREFIX_PATH=<repo>/build/install
    cmake --build build/consumer --config Release
    build/consumer/Release/qobs-downstream-consumer.exe

Result: the downstream project is a separate CMake project that shares no build
tree with the library. It finds the package, links the exported targets, and
runs an end-to-end observation path:

    queue-observatory version 1.0.0
    persistence format 1, wire format 1, policy 1
    accepted=3 rejected=0
    state=elevated freshness=fresh
    export rows=1 bytes=558
    snapshot queues=1 bytes=1086
    transport frame kind=batch
    downstream consumer ok

## 6. Real inter-process transport

The transport suite creates a listening socket, marks it inheritable, starts
`qobs-transport-server` as an independent operating-system process that
inherits it, connects over loopback TCP as a client, sends a canonical document,
and waits for the server process to exit on its own batch budget. A second test
does the mirror image: the suite serves in-process and starts
`qobs-transport-client` as an independent process that sends a document and
exits with a status the suite checks.

Result: both pass. The processes are real, the TCP connection is real, and no
part of the exchange depends on a timeout.

## 7. Examples

    build/release/examples/Release/qobs-example-observe.exe
    build/release/examples/Release/qobs-example-policy.exe
    build/release/examples/Release/qobs-example-history.exe

Result: all three run and print their reports. The history example prints an
insufficient-coverage record for its microburst query, which is the honest
answer: its forty samples land in a single one-second bucket, and the runtime
refuses to draw a burst conclusion from one covered bucket.

## 8. Benchmarks

    build/release/benchmarks/Release/qobs-benchmark.exe
    build/release/benchmarks/Release/qobs-benchmark-transport.exe

Result: every case reports the number of operations that completed. The numbers
recorded in the README are from this run.

## 9. Fresh-clone closure

    git clone --branch v1.0.0 https://github.com/summonlabs/Queue-Observatory.git <clone>
    cmake -S <clone> -B <clone>/build/release -G "Visual Studio 17 2022" -A x64 -DQOBS_WARNINGS_AS_ERRORS=ON
    cmake --build <clone>/build/release --config Release --parallel
    ctest --test-dir <clone>/build/release -C Release
    cmake --install <clone>/build/release --config Release --prefix <clone>/install
    cmake -S <clone>/tests/downstream -B <clone>/build/consumer -DCMAKE_PREFIX_PATH=<clone>/install
    cmake --build <clone>/build/consumer --config Release
    <clone>/build/consumer/Release/qobs-downstream-consumer.exe

Result: the clone resolves to the tagged closure commit, contains 109 files,
configures, builds with zero first-party warnings, passes all seven suites, and
its independently consumed package runs the downstream consumer to completion.

## 10. Defects found and repaired during hardening

| Defect | How it was found | Repair |
| --- | --- | --- |
| The JSON writer accepted only one member per object, so every encoded document was truncated | Encoder round-trip tests failed | The writer now tracks whether a member value is pending, and separators are written once, by the member name |
| The persistence segment header declared 64 bytes while the encoder wrote 80, silently truncating the payload checksum | Recovery round trip reported a payload checksum mismatch | The header size is now written as the sum of its fields, and the header builder fails loudly rather than truncating |
| The persistence record prefix was written as nine bytes but indexed as twelve | Recovery reported a record checksum mismatch on the first record | The prefix is now a fixed eight bytes and takes part in the record checksum |
| Persisted samples lost their source incarnation, so recovered evidence failed identity validation | Recovery admitted zero samples | The incarnation token is persisted, and the snapshot resolves it from the source registry |
| Wire-ingested samples were never stamped with a receive time, so they could never be fresh | Microburst coverage collapsed to one bucket | The store stamps receive time when evidence arrives without one |
| A counter reporting a value wider than its declared width corrupted the running total through an underflowing subtraction | A seeded randomized invariant failed | Such a reading is refused and the baseline is left untouched |
| Queue index zero was treated as an invalid identity, so every queue numbered from zero was rejected | Store tests reported zero queues | Validity is decided by device and port only |
| The classifier required an occupancy measurement even when a depth basis was configured | Classification tests reported unknown | The policy distinguishes required fields from a required-any set, and the basis resolution respects the reported-field mask |
| The canonical encoder would happily emit a record its own decoder would reject as malformed | An example silently produced an undecodable document | The encoder validates the sample identity and refuses to emit an incomplete record |
| The transport server never started the platform networking layer when it adopted an inherited socket | The independent server process failed with a socket error | Socket initialisation is performed explicitly on adopt and before serving |
| Assertion helpers bound references into temporaries | AddressSanitizer | The helpers are functions, so temporaries live for the whole comparison |
| The ignore file carried a bare @@core@@ pattern intended for crash dumps, which also matched @@src/core/@@ and @@include/qobs/core/@@; the entire core layer was silently excluded from the commit | The fresh-clone closure build failed with a missing source file | The crash-dump patterns are now narrow (@@/core@@, @@core.[0-9]*@@, @@*.core@@), and closure is verified by cloning the published tag and building it |

## 11. What is not proven

* No switch, ASIC, NIC, RDMA, InfiniBand, NVLink or multi-host behaviour was
  exercised, because none is implemented.
* ThreadSanitizer was not run; no equivalent race-detection coverage is claimed.
* The benchmark numbers are a record of one run on one machine.
