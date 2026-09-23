# Concurrency, lock ordering and reentrancy audit

Queue Observatory runs background workers and serves concurrent readers. This
document is the audit of how that is made safe, and how the claim is checked
rather than asserted.

## Lock ranks

Every lock in the runtime is assigned a rank. The ranks are defined in
`include/qobs/core/LockAudit.hpp`.

| Rank | Name | What it guards |
| --- | --- | --- |
| 1 | `leaf` | Independent leaf mutexes: the runtime's pending-document queue, the persistence directory lock |
| 10 | `sources` | Reserved for a source registry held independently of the store |
| 20 | `store` | The evidence store's single shared mutex |
| 30 | `persistence` | Persistence segment bookkeeping |
| 40 | `transport` | Reserved for transport-level state |
| 50 | `worker` | Reserved for thread-management state |

## The rules

1. A thread acquires locks in strictly increasing rank order.
2. Rank `leaf` may be acquired while holding any rank, and any rank may be
   acquired while holding only leaf ranks. Leaf locks never acquire another lock
   while held.
3. No non-leaf rank may be acquired while the same rank is already held by the
   same thread. There is no same-rank nesting and no recursion.
4. No lock is held across a callback boundary. Every callback a caller supplies
   is invoked after all locks have been released.

## How the rules are enforced

The audit is compiled in unless `QOBS_ENABLE_LOCK_AUDIT` is off. It keeps a
thread-local stack of held ranks and four global counters:

| Counter | Meaning |
| --- | --- |
| `descending_acquisitions` | A lock was acquired while a higher rank was already held |
| `same_rank_acquisitions` | A non-leaf rank was acquired twice by the same thread |
| `callbacks_under_lock` | A callback boundary was crossed while a non-leaf rank was held |
| `unbalanced_releases` | A release did not match the innermost acquisition |

The audit never throws and never aborts. Violations are recorded so a test can
assert the count is zero, and so that a deliberately provoked violation can be
observed — which keeps the detector itself under test.

Two tests in `qobs_unit_tests` assert that the detector works
(`lock_audit.descending_acquisition_is_detected`,
`lock_audit.same_rank_nesting_is_detected`,
`lock_audit.callback_under_lock_is_detected`,
`lock_audit.unbalanced_release_is_detected`), and two tests assert that normal
operation produces no violation at all
(`lock_audit.ordered_acquisition_is_clean`,
`runtime.lock_audit_records_no_violation_during_normal_operation`,
`concurrency.store_serves_parallel_readers_without_a_lock_violation`).

## Ownership

| Object | Ownership | Concurrency |
| --- | --- | --- |
| `QueueStore` | Exclusive writer, shared readers | One `std::shared_mutex`. Ingest takes it exclusively; every query takes it shared |
| `HistoryRing` | Owned by the store, touched only under the store's exclusive lock | Not independently synchronised by design |
| `PersistenceStore` | Owned by the runtime | One leaf mutex for the directory and its segment accounting |
| `Observatory` pending queue | Owned by the runtime | One leaf mutex plus two condition variables |
| `TransportServer` | Owned by the caller | One mutex for statistics, one per-call queue mutex, one worker pool |

## Why the lock order is satisfiable

The runtime only ever needs to hold a leaf lock and the store lock at the same
time, in that order, which satisfies rule 2. Specifically, a worker thread holds
the pending-queue mutex (rank 1) while applying a document to the store (rank
20). It never takes the store lock and then the pending-queue mutex.

Persistence is only reached from the runtime's own lifecycle methods, which hold
no other lock at that moment: `persist_now` and `recover` take the
persistence lock (rank 30) alone, after releasing the pending-queue mutex. There
is therefore no cycle in the lock graph, and the audit confirms it at runtime.

## Reentrancy

Three places could have invited reentrancy, and each was resolved explicitly:

* **Query results.** Every query copies its result out under the lock and returns
  by value. No reference into store-internal storage escapes.
* **Snapshot iteration.** `QueueStore::for_each_queue` extracts its payload
  under the shared lock, releases it, then invokes the caller's visitor. The
  visitor therefore cannot observe the store locked, and cannot deadlock by
  calling back into it.
* **Reports.** Every renderer in `qobs/runtime/Report.hpp` takes a plain result
  value. No renderer touches the store.

## Shutdown

`Observatory::stop` requests cancellation, notifies every worker, joins every
thread, and only then discards queued documents, counting what it dropped. There
is no detached thread anywhere in the runtime, and no timeout is involved in
shutdown or in any test.
