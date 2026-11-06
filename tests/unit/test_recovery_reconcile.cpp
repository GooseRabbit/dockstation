#include "core/recovery.hpp"

#include <cstdlib>
#include <iostream>

using namespace dockstation::core;

namespace {
int checks = 0;
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
RecoverySubsystemReport safe_lid{"lid", RecoveryDataQuality::Valid, RecoverySubsystemCondition::Safe};
RecoverySubsystemReport safe_charging{"charging", RecoveryDataQuality::Valid, RecoverySubsystemCondition::Safe};
OperationRecord started_operation{{"op-1", "cmd-1", "lid", "close", {}},
                                 OperationLifecycleStatus::Started, 7};
RecoveryOperationReport completed_report{"op-1", "cmd-1", RecoveryDataQuality::Valid,
                                          RecoveryOperationOutcome::Completed};
RecoveryReports make_reports(const RecoverySubsystemReport* subsystems, std::size_t subsystem_count,
                             const OperationRecord* operations, std::size_t operation_count,
                             const RecoveryOperationReport* operation_reports,
                             std::size_t operation_report_count) {
    return {RecoveryCollectionStatus::Complete, subsystems, subsystem_count,
            RecoveryCollectionStatus::Complete, operations, operation_count,
            operation_reports, operation_report_count};
}
}

int main() {
    const StationStateSnapshot snapshot{StationState::EnclosedService, 42, "transition-42"};
    RecoverySubsystemReport subsystems[]{safe_lid, safe_charging};
    OperationRecord operations[]{started_operation};
    RecoveryOperationReport operation_reports[]{completed_report};
    RecoveryReconciliationResult result{};

    result = reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1,
                                                             operation_reports, 1));
    expect(result.status == RecoveryReconciliationStatus::EvidenceComplete,
           "complete safe evidence is classified without changing station state");
    expect(result.checked_subsystem_count == 2 && result.checked_operation_count == 1,
           "validated evidence counts are returned");

    result = reconcile_after_restart(snapshot, make_reports(subsystems, 2, nullptr, 0, nullptr, 0));
    expect(result.status == RecoveryReconciliationStatus::EvidenceComplete,
           "empty unfinished-operation list is valid when collection is complete");

    result = reconcile_after_restart({StationState::PoweredOff, 0, {}},
                                     make_reports(subsystems, 2, nullptr, 0, nullptr, 0));
    expect(result.status == RecoveryReconciliationStatus::EvidenceComplete,
           "initial revision-zero snapshot is valid");
    expect(reconcile_after_restart({static_cast<StationState>(255), 1, "t"},
             make_reports(subsystems, 2, nullptr, 0, nullptr, 0)).status ==
             RecoveryReconciliationStatus::InvalidInput, "unknown snapshot state rejected");
    expect(reconcile_after_restart({StationState::Waiting, 1, {}},
             make_reports(subsystems, 2, nullptr, 0, nullptr, 0)).status ==
             RecoveryReconciliationStatus::InvalidInput, "positive revision requires transition ID");
    expect(reconcile_after_restart({StationState::Waiting, 0, "unexpected"},
             make_reports(subsystems, 2, nullptr, 0, nullptr, 0)).status ==
             RecoveryReconciliationStatus::InvalidInput, "zero revision cannot carry transition ID");
    expect(reconcile_after_restart({StationState::Waiting, 2, " \t"},
             make_reports(subsystems, 2, nullptr, 0, nullptr, 0)).status ==
             RecoveryReconciliationStatus::InvalidInput, "whitespace-only snapshot transition ID rejected");

    RecoverySubsystemReport blank_subsystem_id{" \t", RecoveryDataQuality::Valid,
                                                RecoverySubsystemCondition::Safe};
    expect(reconcile_after_restart(snapshot,
             make_reports(&blank_subsystem_id, 1, nullptr, 0, nullptr, 0)).status ==
             RecoveryReconciliationStatus::InvalidInput, "whitespace-only subsystem ID rejected");

    OperationRecord blank_operation_id = started_operation;
    blank_operation_id.request.operation_id = " \t";
    expect(reconcile_after_restart(snapshot,
             make_reports(subsystems, 2, &blank_operation_id, 1, nullptr, 0)).status ==
             RecoveryReconciliationStatus::InvalidInput, "whitespace-only saved operation ID rejected");

    OperationRecord blank_correlation_id = started_operation;
    blank_correlation_id.request.correlation_id = " \t";
    expect(reconcile_after_restart(snapshot,
             make_reports(subsystems, 2, &blank_correlation_id, 1, nullptr, 0)).status ==
             RecoveryReconciliationStatus::InvalidInput, "whitespace-only saved correlation ID rejected");

    RecoveryOperationReport blank_report_id{" \t", "cmd-1", RecoveryDataQuality::Valid,
                                               RecoveryOperationOutcome::Completed};
    expect(reconcile_after_restart(snapshot,
             make_reports(subsystems, 2, nullptr, 0, &blank_report_id, 1)).status ==
             RecoveryReconciliationStatus::InvalidInput, "whitespace-only operation report ID rejected");

    RecoveryReports reports = make_reports(subsystems, 2, operations, 1, operation_reports, 1);
    reports.subsystem_collection = RecoveryCollectionStatus::Incomplete;
    expect(reconcile_after_restart(snapshot, reports).status ==
           RecoveryReconciliationStatus::Incomplete, "incomplete subsystem collection rejected");
    expect(reconcile_after_restart(snapshot, make_reports(nullptr, 0, nullptr, 0, nullptr, 0)).status ==
           RecoveryReconciliationStatus::Incomplete, "no subsystem reports is incomplete");

    const RecoveryDataQuality bad_quality[]{RecoveryDataQuality::Stale, RecoveryDataQuality::Unknown,
        RecoveryDataQuality::Unavailable, RecoveryDataQuality::Invalid, RecoveryDataQuality::Error};
    for (RecoveryDataQuality quality : bad_quality) {
        RecoverySubsystemReport bad{"lid", quality, RecoverySubsystemCondition::Safe};
        expect(reconcile_after_restart(snapshot, make_reports(&bad, 1, nullptr, 0, nullptr, 0)).status ==
               RecoveryReconciliationStatus::Incomplete, "non-VALID subsystem report is incomplete");
    }
    RecoverySubsystemReport unknown_condition{"lid", RecoveryDataQuality::Valid,
                                               RecoverySubsystemCondition::Unknown};
    expect(reconcile_after_restart(snapshot, make_reports(&unknown_condition, 1, nullptr, 0, nullptr, 0)).status ==
           RecoveryReconciliationStatus::Incomplete, "unknown subsystem condition is incomplete");
    RecoverySubsystemReport invalid_quality{"lid", static_cast<RecoveryDataQuality>(255),
                                              RecoverySubsystemCondition::Safe};
    expect(reconcile_after_restart(snapshot, make_reports(&invalid_quality, 1, nullptr, 0, nullptr, 0)).status ==
           RecoveryReconciliationStatus::InvalidInput, "unknown quality enum rejected");

    RecoverySubsystemReport unsafe{"lid", RecoveryDataQuality::Valid, RecoverySubsystemCondition::Unsafe};
    expect(reconcile_after_restart(snapshot, make_reports(&unsafe, 1, nullptr, 0, nullptr, 0)).status ==
           RecoveryReconciliationStatus::SubsystemUnsafe, "current unsafe subsystem is surfaced");
    RecoverySubsystemReport stale_unsafe{"lid", RecoveryDataQuality::Stale, RecoverySubsystemCondition::Unsafe};
    expect(reconcile_after_restart(snapshot, make_reports(&stale_unsafe, 1, nullptr, 0, nullptr, 0)).status ==
           RecoveryReconciliationStatus::Incomplete, "stale unsafe report is not treated as current fact");

    RecoverySubsystemReport duplicate_subsystems[]{safe_lid, safe_lid};
    expect(reconcile_after_restart(snapshot, make_reports(duplicate_subsystems, 2, nullptr, 0, nullptr, 0)).status ==
           RecoveryReconciliationStatus::Conflict, "duplicate subsystem report IDs conflict");
    expect(reconcile_after_restart(snapshot, make_reports(nullptr, 1, nullptr, 0, nullptr, 0)).status ==
           RecoveryReconciliationStatus::InvalidInput, "null pointer with nonzero count rejected");
    reports = make_reports(subsystems, 2, nullptr, 0, nullptr, 0);
    reports.operation_collection = static_cast<RecoveryCollectionStatus>(255);
    expect(reconcile_after_restart(snapshot, reports).status ==
           RecoveryReconciliationStatus::InvalidInput, "unknown collection status rejected");

    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1, nullptr, 0)).status ==
           RecoveryReconciliationStatus::Incomplete, "missing unfinished-operation report is incomplete");
    RecoveryOperationReport extra{"op-extra", "cmd-extra", RecoveryDataQuality::Valid,
                                    RecoveryOperationOutcome::Completed};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1, &extra, 1)).status ==
           RecoveryReconciliationStatus::Conflict, "unmatched operation ID conflicts even when counts match");
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, nullptr, 0, &extra, 1)).status ==
           RecoveryReconciliationStatus::Conflict, "unmatched operation report conflicts");
    RecoveryOperationReport duplicates[]{completed_report, completed_report};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1, duplicates, 2)).status ==
           RecoveryReconciliationStatus::Conflict, "duplicate operation reports conflict");
    RecoveryOperationReport bad_correlation{"op-1", "other", RecoveryDataQuality::Valid,
                                              RecoveryOperationOutcome::Completed};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1,
                                                           &bad_correlation, 1)).status ==
           RecoveryReconciliationStatus::Conflict, "correlation mismatch conflicts");
    OperationRecord duplicate_ops[]{started_operation, started_operation};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, duplicate_ops, 2,
                                                           operation_reports, 1)).status ==
           RecoveryReconciliationStatus::Conflict, "duplicate saved operation IDs conflict");

    RecoveryOperationReport stale_operation{"op-1", "cmd-1", RecoveryDataQuality::Stale,
                                               RecoveryOperationOutcome::Completed};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1,
                                                           &stale_operation, 1)).status ==
           RecoveryReconciliationStatus::Incomplete, "stale operation report is incomplete");
    RecoveryOperationReport unknown_operation{"op-1", "cmd-1", RecoveryDataQuality::Valid,
                                                 RecoveryOperationOutcome::Unknown};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1,
                                                           &unknown_operation, 1)).status ==
           RecoveryReconciliationStatus::Incomplete, "unknown operation outcome remains unresolved");
    RecoveryOperationReport in_progress{"op-1", "cmd-1", RecoveryDataQuality::Valid,
                                           RecoveryOperationOutcome::InProgress};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1,
                                                           &in_progress, 1)).status ==
           RecoveryReconciliationStatus::OperationInProgress, "in-progress operation is not resumed");

    const RecoveryOperationOutcome settled[]{RecoveryOperationOutcome::NotExecuted,
        RecoveryOperationOutcome::Completed, RecoveryOperationOutcome::Failed,
        RecoveryOperationOutcome::Cancelled};
    for (RecoveryOperationOutcome outcome : settled) {
        RecoveryOperationReport report{"op-1", "cmd-1", RecoveryDataQuality::Valid, outcome};
        OperationRecord matching_saved = started_operation;
        if (outcome == RecoveryOperationOutcome::NotExecuted) {
            matching_saved.status = OperationLifecycleStatus::Accepted;
        }
        expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, &matching_saved, 1,
                                                               &report, 1)).status ==
               RecoveryReconciliationStatus::EvidenceComplete, "explicit settled operation outcome accepted");
    }

    OperationRecord terminal = started_operation;
    terminal.status = OperationLifecycleStatus::Completed;
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, &terminal, 1,
                                                           operation_reports, 1)).status ==
           RecoveryReconciliationStatus::InvalidInput, "terminal record is not an unfinished operation");
    OperationRecord no_id = started_operation;
    no_id.request.operation_id = {};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, &no_id, 1,
                                                           operation_reports, 1)).status ==
           RecoveryReconciliationStatus::InvalidInput, "saved operation ID is required");
    RecoveryOperationReport invalid_outcome{"op-1", "cmd-1", RecoveryDataQuality::Valid,
                                               static_cast<RecoveryOperationOutcome>(255)};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1,
                                                           &invalid_outcome, 1)).status ==
           RecoveryReconciliationStatus::InvalidInput, "unknown operation outcome enum rejected");
    RecoveryOperationReport contradiction{"op-1", "cmd-1", RecoveryDataQuality::Valid,
                                             RecoveryOperationOutcome::NotExecuted};
    expect(reconcile_after_restart(snapshot, make_reports(subsystems, 2, operations, 1,
                                                           &contradiction, 1)).status ==
           RecoveryReconciliationStatus::Conflict, "STARTED conflicts with valid NOT_EXECUTED report");

    std::cout << "PASS: " << checks << " assertions\n";
}
