# Source-session confirmation: verification scenarios

**Status:** approved architecture and scenario specification (2026-11-14).
**Execution status:** host harness invokes the production `SourceSessionCoordinator` with fake ports. This verifies core orchestration only; it does not claim that production source-session verification, a trusted registry, or persistence exists.

## Scope and test boundary

The scenarios cover orchestration between a trusted source registry,
`SourceSessionVerifier`, `SourceCursorReader`, `SourceSessionRepository`,
and the event adapter before F13.

The host harness uses deterministic fake verifier/reader/repository ports at
the external adapter boundaries and invokes the real `SourceSessionCoordinator`.
It exercises status handling, cursor compare-and-set, registration-revision
checks, and event/evidence binding. The fakes establish only software
control-flow behavior. They cannot prove challenge-response cryptography, key
protection, provisioning, actual device identity, database durability,
process-crash recovery, or Repka/ESP32 transport behavior.

The existing `tests/` directory must remain untouched. New executable checks,
when the public API is approved, belong under `verification/regression/` and
must be registered separately with CTest.

## Required scenarios

| ID | Scenario | Expected result / invariant |
|---|---|---|
| SSR-01 | Event or handshake names an unregistered logical source | Verifier returns `UnknownSource`; repository is not called; no cursor is created; event does not reach F13. |
| SSR-02 | Registered source is `DISABLED` or `REVOKED` | Verification is rejected; no cursor mutation and no event processing. |
| SSR-03 | Peer identity does not match the identity reference bound to the registered source | `Rejected`; source text, Station ID, MAC/IP or event payload alone cannot repair the mismatch. |
| SSR-04 | Session establishment proof is stale/replayed on a different channel, or is not bound to this source/session | `Rejected`; no cursor mutation. Reuse within the same still-active channel is covered by SSR-19. |
| SSR-05 | Verifier returns `Unavailable`, `Error` or an unknown status | Fail closed; repository is not called; no event processing. |
| SSR-06 | Cursor is `NotFound`, source is registered/active and verifier returns `Verified` | Repository atomically creates cursor with exact source and verified source_instance; last_sequence is empty; result `Confirmed`. |
| SSR-07 | Cursor is `NotFound`, but source/session is unverified | No initial cursor is created; event is blocked. |
| SSR-08 | Cursor is `Found` with the same source_instance as the verified session | Result `AlreadyCurrent`; existing last_sequence is preserved exactly, including its absence. |
| SSR-09 | Cursor is `Found` with a different source_instance and verifier confirms a fresh session | In one transaction, replace source_instance and clear last_sequence; result `Confirmed`. |
| SSR-10 | Cursor reader returns `Error`, malformed `Found`/`NotFound`, or unknown status | No repository commit and no event processing. Error must never be converted into NotFound. |
| SSR-11 | Cursor changes after it was read but before confirmation commit | Repository returns `CursorChanged`; it does not overwrite the newer cursor. |
| SSR-12 | Two session confirmations race using the same expected cursor snapshot | At most one CAS succeeds; the loser gets `CursorChanged`; no silent overwrite or blind retry. |
| SSR-13 | Source is revoked/disabled or its registration revision changes after verification but before commit | Repository checks the current registry state inside the same transaction, returns `RegistrationChanged`, and leaves cursor unchanged. |
| SSR-14 | Event source or source_instance differs from the session evidence supplied by the trusted channel adapter | `SourceSessionCoordinator` returns `InvalidEvidence` before calling verifier, reader or repository; values in the event body cannot switch the session. |
| SSR-15 | Delayed event from the previous source_instance arrives after a new session was confirmed | F04 reports `UNORDERED / SourceInstanceMismatch`; no profile commit. |
| SSR-16 | Event from a session-capable source omits source_sequence | F04 reports `UNORDERED / SequenceMissing`; timestamp is not used as a fallback. |
| SSR-17 | Repository returns `Error`, an unknown status, or contradictory result data | Fail closed; do not treat the session as confirmed and do not pass events to F13. |
| SSR-18 | Source does not support source_instance | The current session-gated Pipeline returns InvalidEvidence and does not call verifier/reader/repository or F13. The generic F04 freshness function may still represent a cursor without instance when used independently, but this does not authorize bypassing session verification. A legacy source needs a separately approved authenticated-session contract. |
| SSR-19 | Several events are received over the same continuously open, authenticated channel | Rechecking the channel-bound session context for each event is idempotent; the same established proof is not misclassified as replay while that channel remains active; AlreadyCurrent preserves last_sequence. Reuse on a new/closed channel is rejected. |
| SSR-20 | Event envelope is invalid or event_type is unknown before session confirmation | Pipeline returns InvalidEvent/UnknownEventType with no session result; verifier, cursor reader and session repository are not called and no cursor is created/replaced. |

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
7. The transport adapter builds Event and SourceSessionEvidence from one
   authenticated receive context. Matching source/source_instance strings
   are necessary but not sufficient to establish this binding.
8. SourceSessionVerifier is called for each event. It must distinguish a
   repeated check on the same live authenticated channel from replay on a
   different/closed channel; new channels require fresh session establishment.
9. Pipeline performs pure F01/F02 checks before session confirmation so
   malformed/unknown events cannot create or replace the persisted session cursor.
   After confirmation, handle_event repeats F01/F02 and then runs F03/F04;

## Open implementation decisions

The C++ coordinator signature and its fail-closed execution order are approved
and implemented. The matrix does not select the registry schema, database,
provisioning process, hardware activation method, cryptographic algorithms,
challenge-response protocol, protected transport, source_instance generation
strategy, or Repka—ESP32 link. These remain separate design decisions.

The host harness is registered as CTest target `source_session_contract` and
reports its executed check count. The regression matrix includes SSR-19 (repeated
validation on the same live channel) and SSR-20 (Pipeline preflight edge cases). This scenario specification itself is not
an executed test and is not evidence of production security.


## Implemented host harness boundary

The CTest target `source_session_contract` exercises all 18 scenario IDs
through the production `SourceSessionCoordinator`. SSR-14 verifies the
coordinator rejects mismatched Event/evidence before the verifier, reader or
repository is called. The fake repository models the required compare-and-set
and registration-revision checks but is not evidence of a real transaction or
durable storage. A production channel adapter and F13 composition-root
integration remain open.
