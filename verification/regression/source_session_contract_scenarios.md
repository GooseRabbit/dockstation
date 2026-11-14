# Source-session confirmation: verification scenarios

**Status:** approved architecture and scenario specification (2026-11-14).
**Execution status:** host harness implemented in `test_source_session_contract.cpp`; CI execution is pending. The scenarios describe test-double expectations and do not claim that production source-session verification or persistence exists.

## Scope and test boundary

The scenarios cover orchestration between a trusted source registry,
`SourceSessionVerifier`, `SourceCursorReader`, `SourceSessionRepository`,
and the event adapter before F13.

A future host harness may use deterministic fake verifier/repository ports to
exercise status handling, cursor compare-and-set, registration-revision checks,
and event/session binding. Such fakes can establish only software control-flow
behavior. They cannot prove challenge-response cryptography, key protection,
provisioning, actual device identity, database durability, process-crash
recovery, or Repka/ESP32 transport behavior.

The existing `tests/` directory must remain untouched. New executable checks,
when the public API is approved, belong under `verification/regression/` and
must be registered separately with CTest.

## Required scenarios

| ID | Scenario | Expected result / invariant |
|---|---|---|
| SSR-01 | Event or handshake names an unregistered logical source | Verifier returns `UnknownSource`; repository is not called; no cursor is created; event does not reach F13. |
| SSR-02 | Registered source is `DISABLED` or `REVOKED` | Verification is rejected; no cursor mutation and no event processing. |
| SSR-03 | Peer identity does not match the identity reference bound to the registered source | `Rejected`; source text, Station ID, MAC/IP or event payload alone cannot repair the mismatch. |
| SSR-04 | Session proof/challenge is replayed, stale or not bound to this source/session | `Rejected`; no cursor mutation. |
| SSR-05 | Verifier returns `Unavailable`, `Error` or an unknown status | Fail closed; repository is not called; no event processing. |
| SSR-06 | Cursor is `NotFound`, source is registered/active and verifier returns `Verified` | Repository atomically creates cursor with exact source and verified source_instance; last_sequence is empty; result `Confirmed`. |
| SSR-07 | Cursor is `NotFound`, but source/session is unverified | No initial cursor is created; event is blocked. |
| SSR-08 | Cursor is `Found` with the same source_instance as the verified session | Result `AlreadyCurrent`; existing last_sequence is preserved exactly, including its absence. |
| SSR-09 | Cursor is `Found` with a different source_instance and verifier confirms a fresh session | In one transaction, replace source_instance and clear last_sequence; result `Confirmed`. |
| SSR-10 | Cursor reader returns `Error`, malformed `Found`/`NotFound`, or unknown status | No repository commit and no event processing. Error must never be converted into NotFound. |
| SSR-11 | Cursor changes after it was read but before confirmation commit | Repository returns `CursorChanged`; it does not overwrite the newer cursor. |
| SSR-12 | Two session confirmations race using the same expected cursor snapshot | At most one CAS succeeds; the loser gets `CursorChanged`; no silent overwrite or blind retry. |
| SSR-13 | Source is revoked/disabled or its registration revision changes after verification but before commit | Repository checks the current registry state inside the same transaction, returns `RegistrationChanged`, and leaves cursor unchanged. |
| SSR-14 | Event source or source_instance differs from the authenticated channel context | Adapter rejects the event before F13; values in the event body cannot switch the session. |
| SSR-15 | Delayed event from the previous source_instance arrives after a new session was confirmed | F04 reports `UNORDERED / SourceInstanceMismatch`; no profile commit. |
| SSR-16 | Event from a session-capable source omits source_sequence | F04 reports `UNORDERED / SequenceMissing`; timestamp is not used as a fallback. |
| SSR-17 | Repository returns `Error`, an unknown status, or contradictory result data | Fail closed; do not treat the session as confirmed and do not pass events to F13. |
| SSR-18 | Source does not support source_instance | No automatic session reset is inferred. The applicable source contract must define ordering; absent source_sequence remains unordered under F04. |

## Cross-scenario invariants

1. Registration is provisioned through a trusted process; an incoming event can
   never register a source or replace its identity binding.
2. Station ID is the permanent station identifier, not a secret or standalone
   proof of controller identity. `source` is a logical registered source;
   `source_instance` identifies one runtime/session and is not a credential.
3. A `Found` expected cursor is compared using all fields:
   `source`, `source_instance`, and `last_sequence`. An expected
   `NotFound` requires that the row is still absent.
4. A repeated confirmation of the current instance must not clear
   `last_sequence`. A different instance is installed only after independent
   verification and an atomic compare-and-set.
5. The repository must validate the registration's active state and revision
   within the commit transaction, closing the revoke-between-verify-and-commit
   race.
6. All unknown, unavailable, malformed, contradictory or failed results block
   event processing. No implicit `NotFound`, default sequence, timestamp
   ordering, or blind retry is permitted.
7. After session confirmation, the event adapter binds the event to the
   authenticated channel context before F13. Existing duplicate/freshness
   checks and F09/F10 atomic cursor advancement remain in force.

## Open implementation decisions

The scenario matrix does not select the C++ method signatures, memory ownership
model, registry schema, database, provisioning process, hardware activation
method, cryptographic algorithms, challenge-response protocol, protected
transport, source_instance generation strategy, or Repka—ESP32 link. These must
be resolved in the relevant design review before implementation.

The host harness is registered as CTest target `source_session_contract` and
reports its executed check count. This scenario specification itself is not
an executed test and is not evidence of production security.


## Implemented host harness boundary

The CTest target `source_session_contract` uses fake ports and a test-only
reference coordinator. It exercises all 18 scenario IDs and the public
result validators. SSR-14 uses a test-local source/session binding predicate;
there is not yet a production event adapter or F13 composition-root hook.
The fake repository models the required compare-and-set and registration
revision checks but is not evidence of a real transaction or durable storage.
