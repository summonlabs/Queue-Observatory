# Formats

Both formats are versioned independently of the library version. The constants
live in `include/qobs/version.hpp`; the structures live in
`include/qobs/ingest/Wire.hpp` and `include/qobs/persist/Format.hpp`.

## Canonical observation format (wire version 1)

Newline-delimited JSON. One object per line. Blank lines and lines whose first
non-space character is `#` are ignored. A malformed line is counted, described
and skipped; it never hides the lines around it.

Every number is encoded as a JSON integer. No measurement is ever encoded as a
floating point value, in either direction.

### `queue_sample`

| Field | Required | Notes |
| --- | --- | --- |
| `kind` | yes | `queue_sample` |
| `v` | no | Defaults to 1. Any other value is reported as a version mismatch |
| `device`, `port` | yes | Identities; must not contain the `|` path separator |
| `queue` | yes | Unsigned, at most 32 bits. Zero is an ordinary queue |
| `source`, `incarnation` | yes | Identities |
| `generation` | yes | Non-zero |
| `sequence` | yes | Non-zero, strictly increasing per source incarnation |
| `authority` | no | `unknown`, `secondary`, `primary`, `authoritative` |
| `observed_ns` | yes | Integer nanoseconds in the named clock domain |
| `clock_domain` | yes | Identity. Times from different domains are never compared |
| `revision` | no | Non-negative integer |
| `traffic_class` | no | Unsigned, at most 16 bits |
| `scheduling_class` | no | Identity |
| measurements | at least one | See below |
| `declared` | no | Array of measurement names the sender intends to report. Defaults to the reported set |

Measurement names: `occupancy_bytes`, `occupancy_cells`,
`queue_depth_packets`, `shared_buffer_cells`, `headroom_cells`,
`reserved_cells`, `static_threshold_cells`, `dynamic_threshold_cells`,
`max_depth_packets`, `high_watermark_cells`, `enqueue_packets`,
`dequeue_packets`, `drop_packets`, `drop_bytes`, `mark_packets`,
`pause_frames`, `pause_duration_nanos`.

A measurement that is absent is absent. It is never treated as zero, and a
sample that reports a field it did not declare is refused.

There is deliberately **no** receive timestamp. Receive time is a fact about the
receiving runtime, so the runtime stamps it when the evidence arrives.

### `class_metadata`

| Field | Required | Notes |
| --- | --- | --- |
| `kind` | yes | `class_metadata` |
| `@v` | no | Defaults to 1 |
| `revision` | yes | Non-zero; revisions are fenced per generation |
| `generation` | yes | Non-zero |
| `source` | yes | The source that supplied the table |
| `authority` | no | Source authority |
| `traffic_classes` | no | Array of `{id, name, dscp, pcp, scheduling_class}` |
| `scheduling_classes` | no | Array of `{id, mode, weight}` |
| `bindings` | no | Array of `{device, port, queue, traffic_class, scheduling_class}` |

## Persistence format (format version 1)

Little-endian throughout. A segment is:

    magic "QOBSSEG1"                 8
    format version                   4
    header size                      4
    header checksum (CRC-32C)        4
    policy version                   4
    runtime identifier length        4
    reserved flags                   4
    created (wall, nanoseconds)      8
    created (steady, nanoseconds)    8
    record count                     8
    payload bytes                    8
    payload checksum (CRC-32C)       4
    writer version major             4
    writer version minor             4
    writer version patch             4
                                     = 80 bytes

The header checksum covers all 80 header bytes with the checksum field zeroed.
The payload checksum covers every payload byte.

Each record is:

    checksum (CRC-32C)               4
    type                             1
    flags                            1
    reserved                         2
    value length                     4
    value                            value length bytes
                                     = 12 + value length

The checksum covers the eight prefix bytes and the value, so a damaged length
field is detected as well as a damaged payload.

Record types: `SessionHeader` (1), `QueueSnapshot` (2), `EventBatch` (3),
`SourceRegistry` (4), `MetadataTable` (5).

### Recovery rules

* Wrong magic, wrong header size, bad header checksum or an unsupported format
  version: the segment is refused and the recovery report says which.
* A record whose stated length runs past the end of the payload, or whose
  checksum does not match: decoding stops there. Everything read before it is
  returned, and the report records how many records were parsed, how many were
  rejected and what the damage was.
* A segment that is smaller than its header, or whose runtime identifier runs
  past the end of the file, is treated as truncated.
* The payload checksum is verified independently and its result is reported even
  when individual records decoded.
* Recovered evidence is re-admitted with the recovered provenance and a capped
  freshness. It can never define a current state on its own.

Segment files are named `segment-NNNNNNNN.qobs`. Writes go to
`segment-NNNNNNNN.qobs.tmp`, are flushed and synced, and are then renamed. The
newest segment that decodes with a valid header is used; older segments are
tried only when the newest cannot be read.
