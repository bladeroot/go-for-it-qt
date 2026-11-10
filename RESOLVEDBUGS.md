# Resolved Bugs Registry

This document maintains a comprehensive, structured record of defects, edge cases, and architectural regressions identified and resolved in **Go for It Qt** (`go-for-it-qt`, `go-for-it-tui`).

All entries are validated by regression tests in [tests/tst_todotxt.cpp](file:///home/bladeroot/prj/other/Go-For-It/tests/tst_todotxt.cpp).

---

## Table of Contents

1. [Part 1: Foundational Defects & Architectural Hardening (Pre-KB)](#part-1-foundational-defects--architectural-hardening-pre-kb)
   - [1.1. SQLite Persistence, Transaction Atomicity & Reordering](#11-sqlite-persistence-transaction-atomicity--reordering)
   - [1.2. Timer Integrity, Activity Logging & Anti-Leakage](#12-timer-integrity-activity-logging--anti-leakage)
   - [1.3. Remote Issue Tracker Synchronization](#13-remote-issue-tracker-synchronization)
   - [1.4. Test Environment Isolation & System Safety](#14-test-environment-isolation--system-safety)
   - [1.5. User Interface (GUI / QML / TUI)](#15-user-interface-gui--qml--tui)
   - [1.6. Packaging & Linux Distribution Compatibility](#16-packaging--linux-distribution-compatibility)
2. [Part 2: Systematic Review Defects (KB-1 through KB-85)](#part-2-systematic-review-defects-kb-1-through-kb-85)
   - [KB-1 – KB-14: Tracker Pagination & Reconciliation](#kb-1--kb-14-tracker-pagination--reconciliation)
   - [KB-15 – KB-29: Scheme Validation, Arithmetic Bounds & List Mutation Safety](#kb-15--kb-29-scheme-validation-arithmetic-bounds--list-mutation-safety)
   - [KB-30 – KB-43: Permissions, Export Failure Handling & TUI Multibyte Input](#kb-30--kb-43-permissions-export-failure-handling--tui-multibyte-input)
   - [KB-44 – KB-56: Bearer Auth, Priority Mappings & Timer Reliability](#kb-44--kb-56-bearer-auth-priority-mappings--timer-reliability)
   - [KB-57 – KB-69: Nextcloud CalDAV, Pomodoro Continuity & Token Masking](#kb-57--kb-69-nextcloud-caldav-pomodoro-continuity--token-masking)
   - [KB-70 – KB-76: TUI Formatting, ANSI Sequences & Dialog Binding](#kb-70--kb-76-tui-formatting-ansi-sequences--dialog-binding)
   - [KB-77 – KB-85: GitHub Queries, TUI Teardown, Heatmap, Redmine, Schedule & TUI Input Clamping](#kb-77--kb-85-github-queries-tui-teardown-heatmap-redmine-schedule--tui-input-clamping)

---

## Part 1: Foundational Defects & Architectural Hardening (Pre-KB)

### 1.1. SQLite Persistence, Transaction Atomicity & Reordering

* **Zero Affected Rows Guard**:
  * *Problem*: In batch reordering and moving completed tasks, a failure on a single row previously left database state silently desynchronized from the in-memory model.
  * *Fix*: Enforced strict verification of `rowsAffected > 0` on every batch update step, rolling back (`ROLLBACK`) on zero affected rows.
  * *Commits*: `a962389`, `458f702`, `77044f1`.
  * *Test*: `testAssertRollbackStateOnFailedBatchReorders`.

* **Sort Order Compaction on Task Deletion**:
  * *Problem*: Deleting tasks left gaps in `sort_order`, resulting in integer exhaustion and erratic ordering on subsequent prepend/insert operations.
  * *Fix*: Implemented atomic compaction of `sort_order` scoped to the current list on task removal.
  * *Commits*: `5f87bc7`, `10c146e`, `a742564`.

* **Transactional Savepoints (`SAVEPOINT`) and Model Rollback**:
  * *Problem*: When SQLite writes failed, the in-memory Qt models had already updated their internal vectors, causing UI and disk state to diverge.
  * *Fix*: Introduced transactional savepoints in [DatabaseService.cpp](file:///home/bladeroot/prj/other/Go-For-It/src/core/DatabaseService.cpp). Model vectors are updated only via callbacks after database commit confirmation, rolling back on failure.
  * *Commits*: `70e5134`, `77bd35c`, `f128e72`, `88d5289`, `136604c`, `7ea3b9e`, `4744c3b`, `65f7878`, `86cd472`.
  * *Tests*: `testStatusNormalizationAndAtomicReorderAndSyncTx`, `testSyncCompletionDateTimerRestorationAndMultiListOrder`.

* **Atomic All-Lists Done Deletion ("Clear Done")**:
  * *Problem*: Clearing completed tasks across all lists could fail mid-way, leaving some lists purged and others untouched.
  * *Fix*: Wrapped multi-list done purging in a single atomic database transaction.
  * *Commit*: `7ea3b9e`.
  * *Test*: `testAllListsClearDoneAndStatusSideEffectsSafety`.

---

### 1.2. Timer Integrity, Activity Logging & Anti-Leakage

* **Timer Tracking by Unique Task ID**:
  * *Problem*: The timer originally tracked tasks by raw model row index. Sorting, filtering, or prepending tasks reassigned the active timer to another task.
  * *Fix*: Bound timer state strictly to `taskId` ([AppController.cpp](file:///home/bladeroot/prj/other/Go-For-It/src/core/AppController.cpp)).
  * *Commit*: `f7825fe`.
  * *Test*: `testTaskTimerTrackingAndDatabaseSafety`.

* **Elapsed Time Leakage Prevention**:
  * *Problem*: Rapidly switching tasks, deleting an active task, or marking it done lost elapsed session seconds before flushing to the database.
  * *Fix*: Guaranteed a synchronous flush of accumulated session time to SQLite prior to any context change or task removal.
  * *Commits*: `c6be0b2`, `205cda2`.
  * *Test*: `testTaskTimerSwitchingAndDatabaseAtomicity`.

* **Preserving Timer State on Non-Final Actions**:
  * *Problem*: Non-final status toggles or transient progress write errors prematurely halted running timers.
  * *Fix*: Confined timer termination strictly to final completion (`Status::Done`) or explicit user stop actions.
  * *Commits*: `f80bfcb`, `c434993`.
  * *Test*: `testDatabaseErrorHandlingAndNonFinalStatusTimer`.

* **Subsecond Inflation Guard**:
  * *Problem*: Frequent start/stop cycles artificially rounded fractional milliseconds up, corrupting activity logging metrics.
  * *Fix*: Added a millisecond accumulator buffer with deterministic whole-second rounding.
  * *Commit*: `f7825fe`.

---

### 1.3. Remote Issue Tracker Synchronization

* **True Multi-Page Pagination**:
  * *Problem*: Synchronizers for Jira, YouTrack, GitHub, GitLab, and Redmine fetched only the initial 50–100 issues due to missing page offset traversal.
  * *Fix*: Implemented recursive asynchronous pagination with accumulated results in [IssueTrackerService.cpp](file:///home/bladeroot/prj/other/Go-For-It/src/core/IssueTrackerService.cpp).
  * *Commit*: `7474d7f`.
  * *Test*: `testPaginationReconciliationForLargeLists`.

* **Lambda Reference Cycle Elimination**:
  * *Problem*: Paginator closures captured shared pointers, preventing garbage collection and leaking memory on repeated sync cycles.
  * *Fix*: Transitioned to weak references (`QPointer` / `std::weak_ptr`) to break ownership cycles.
  * *Commit*: `be76830`.

* **Reconciliation of Remotely Closed Issues on Large Lists**:
  * *Problem*: Issues closed on the server that slipped beyond the first page window caused active local tasks to be closed erroneously due to partial page views.
  * *Fix*: Implemented two-pass reconciliation with full issue key set validation before updating local statuses.
  * *Commits*: `a0a67da`, `d8afb42`, `8cb49bf`, `43aca05`.
  * *Test*: `testRemoteClosedIssuesReconciliation`.

* **Automatic Redmine Closed Status Learning**:
  * *Problem*: Redmine installations use arbitrary IDs and names for closed statuses (e.g., Closed, Resolved, Rejected).
  * *Fix*: Added pre-flight status metadata discovery to dynamically learn closed status IDs per instance.
  * *Commits*: `303ca7d`, `c326227`.

---

### 1.4. Test Environment Isolation & System Safety

* **Test Sandbox Isolation**:
  * *Problem*: Test suites inspected and modified the user's real `~/.config/go-for-it/` and production SQLite database.
  * *Fix*: Sandboxed `HOME`, `XDG_CONFIG_HOME`, and `XDG_DATA_HOME` to isolated `/tmp` directories for all test executions.
  * *Commits*: `8d36acc`, `ff92ed9`.

* **Headless Offscreen Execution & Audio Muting**:
  * *Problem*: CI test runners without X11 or audio subsystems crashed on sound playback and QML component initialization.
  * *Fix*: Defaulted test runs to `QT_QPA_PLATFORM=offscreen` and mocked audio output in headless environments.
  * *Commits*: `8db30bb`, `eba483d`.

* **Do Not Disturb (DND) Crash Recovery**:
  * *Problem*: An abnormal process crash left system notification suppression active indefinitely.
  * *Fix*: Implemented PID-aware `QLockFile` recovery to release stale focus locks on crash.
  * *Commits*: `5465a59`, `0291b46`.

---

### 1.5. User Interface (GUI / QML / TUI)

* **QML Property Warnings & Invalid Shortcuts**:
  * *Problem*: Using the deprecated `shortcut` property on `MenuItem` raised Qt runtime warnings and dropped `Ctrl+M`.
  * *Fix*: Replaced with dedicated `Shortcut` elements.
  * *Commit*: `a0ff6ae`.
* **Missing `currentPriorityMapping` Property**:
  * *Problem*: Opening list settings crashed on accessing undefined priority mapping properties.
  * *Fix*: Declared property explicitly in [TxtListEditDialog.qml](file:///home/bladeroot/prj/other/Go-For-It/qml/dialogs/TxtListEditDialog.qml).
  * *Commit*: `4cf271e`.
* **Exposing Model Helpers to QML**:
  * *Problem*: `entryAt` and `get` were inaccessible to QML views in `ListManagerModel`.
  * *Fix*: Added `Q_INVOKABLE` qualifiers.
  * *Commit*: `1a06d47`.

---

### 1.6. Packaging & Linux Distribution Compatibility

* **Qt 6 Version Matrix (Qt 6.4 through 6.8)**:
  * *Problem*: Supported distributions (Ubuntu 22.04 Jammy, 24.04 Noble, Debian Trixie) ship varying Qt 6 releases with policy differences.
  * *Fix*: Guarded CMake policies (e.g. `QTP0004`), adjusted dependency packages in `debian/control`, and verified clean builds on all targets.
  * *Commits*: `ecb6477`, `fef8d0d`, `715d8ad`, `bf200f3`, `71f0579`.
* **CPack Debian Packaging Adjustments**:
  * *Problem*: `shlibdeps` failed due to missing `file` utility and `dh_auto_test` conflicted with ninja test targets.
  * *Fix*: Added build-essential tools and streamlined `debian/rules`.
  * *Commits*: `81a5a62`, `07a3112`, `23a6364`.

---

## Part 2: Systematic Review Defects (KB-1 through KB-85)

### KB-1 – KB-14: Tracker Pagination & Reconciliation

* **KB-1**: Nextcloud WebDAV — Non-CalDAV XML responses must not be treated as empty task lists that close local tasks. (*Test*: `testNextcloudNonCalDavBodyDoesNotCloseTasks`, commit `96dede1`).
* **KB-2**: Redmine — Pagination shifts caused by concurrent issue creations do not close skipped issues. (*Test*: `testRedminePaginationShiftDoesNotCloseSkippedIssue`, commit `96dede1`).
* **KB-3**: Redmine — Automatic closed status discovery ignores rejected issues to prevent incorrect status mapping. (*Test*: `testRedmineLearnedClosedFallbackSkipsRejected`, commit `96dede1`).
* **KB-4**: GitLab — Guarded against closing tasks when pagination results shift due to incoming issues during sync. (*Test*: `testGitLabPaginationShiftDoesNotCloseSkippedIssue`, commit `96dede1`).
* **KB-5**: YouTrack — Eliminated premature task completion on pagination shifts. (*Test*: `testYouTrackPaginationShiftDoesNotCloseSkippedIssue`, commit `96dede1`).
* **KB-6**: Nextcloud — Multistatus XML responses without `calendar-data` blocks do not close local tasks. (*Test*: `testNextcloudMultistatusWithoutCalendarDataDoesNotCloseTasks`, commit `96dede1`).
* **KB-7**: Nextcloud — Incomplete or malformed VCALENDAR payloads do not close unmentioned tasks. (*Test*: `testNextcloudPartialCalendarDataDoesNotCloseMissingTasks`, commit `96dede1`).
* **KB-8**: YouTrack — Corrected reconciliation of closed issues across multi-page result sets. (*Test*: `testYouTrackMultiPageListStillReconcilesClosedIssue`, commit `96dede1`).
* **KB-9**: YouTrack — Guaranteed reliable reconciliation on large stable lists (>100 tasks). (*Test*: `testYouTrackLargeStableListStillReconcilesClosedIssue`, commit `96dede1`).
* **KB-10**: YouTrack — Handled edge-case reconciliation for tasks positioned on page boundary transitions. (*Test*: `testYouTrackClosedIssueAtPageBoundaryIsReconciled`, commit `96dede1`).
* **KB-11 – KB-14**: YouTrack — Protected against cross-project pagination shifts, ensured synchronization outcome independence from list ID, and implemented two-pass realistic shift detection. (*Tests*: `testYouTrackCrossProjectShiftOnLaterPageDoesNotCloseSkippedIssue`, `testYouTrackSyncOutcomeDoesNotDependOnListId`, `testYouTrackTwoPassDetectsRealisticShift`, commit `fb7e847`).

---

### KB-15 – KB-29: Scheme Validation, Arithmetic Bounds & List Mutation Safety

* **KB-15**: Prevented default demo tasks from being re-added after a user explicitly empties their list. (*Test*: `testDefaultTasksNotReaddedAfterUserEmptiesList`, commit `5edf4cc`).
* **KB-16**: Blocked unsafe URI schemes (e.g. `file://`, shell invocation) from being launched via "Open in Browser". (*Test*: `testOpenUrlExternallyRejectsUnsafeSchemes`, commit `5edf4cc`).
* **KB-17**: Preserved existing working database file when a database relocation/move operation fails. (*Test*: `testFailedDatabaseRelocationKeepsCurrentDatabase`, commit `c77d5b7`).
* **KB-18**: Bounded authentication retries in Jira when both Basic and Bearer auth schemes fail. (*Test*: `testJiraAuthRetryIsBoundedWhenBothSchemesFail`, commit `c77d5b7`).
* **KB-19 – KB-21**: Mock test server header parsing and connection buffering improvements. (*Commits*: `b280825`, `fb43f6c`, `cd0923e`, `7c606aa`).
* **KB-22 – KB-26**: Integer range bounds enforcement in task rows and priority mappings. (*Commits*: `24c0fd2`, `f7b3d91`).
* **KB-27**: Rejected list mutation operations when target row is missing or deleted. (*Test*: `testListMutationsRejectMissingRows`, commit `c816a12`).
* **KB-28**: Automatically dropped externally deleted lists from the UI model upon discovery. (*Test*: `testRemovingListAlreadyDeletedElsewhereDropsItFromModel`, commit `360a0f6`).
* **KB-29**: Hardened duration token parsing against arithmetic integer overflow. (*Test*: `testDurationTokensRejectArithmeticOverflow`, commit `6e2922d`).

---

### KB-30 – KB-43: Permissions, Export Failure Handling & TUI Multibyte Input

* **KB-30**: Enforced strict `0600` permissions (owner-only) on exported billing report files. (*Test*: `testBillingExportFilesAreOwnerOnly`, commit `27dc901`).
* **KB-31**: Reported failure and returned false when PDF/CSV billing exports cannot be written. (*Test*: `testBillingPdfExportReportsFailureWhenTargetCannotBeWritten`, commit `07d9771`).
* **KB-32**: Automatically removed due date metadata (`due:`) when the user deletes the token from task text. (*Test*: `testEditingTextRemovesDueDate`, commit `79cc64d`).
* **KB-33**: Set strict `0600` permissions on the timer activity log (`activity.csv`). (*Test*: `testActivityLogCsvIsOwnerOnly`, commit `79cc64d`).
* **KB-34**: Escaped markup in notification bodies to prevent desktop notification formatting injection. (*Test*: `testNotificationPayloadEscapesMarkup`, `testNotificationTitleIsNotHtmlEscaped`, commit `79cc64d`).
* **KB-35**: Isolated tasks by list in the billing report breakdown view. (*Test*: `testBillingBreakdownKeepsTasksOfDifferentListsApart`, commit `79d5091`).
* **KB-36**: Made remote tracker write operations explicit no-ops in test mode. (*Test*: `testTrackerWritesAreNoOpsInTestMode`, commit `79d5091`).
* **KB-37**: Preserved priority and duration metadata during inline title edits in TUI. (*Test*: `testTuiStyleTitleEditKeepsPriorityAndDuration`, commit `79d5091`).
* **KB-38 – KB-39**: Added full multibyte Unicode and non-BMP character support to TUI input buffers. (*Test*: `testTuiInputKeepsMultibyteAndNonBmpCharacters`, commit `79d5091`).
* **KB-40**: Tokenized keyboard ANSI escape sequences from typed text to prevent display glitching in TUI. (*Test*: `testTuiSplitsEscapeSequencesFromTypedText`, commit `eb30c2c`).
* **KB-41**: Mapped the lowest remote tracker priority level to local priority `(E)`. (*Test*: `testLowestRemotePriorityMapsToE`, commit `eb30c2c`).
* **KB-42**: Propagated and logged remote worklog submission failures. (*Test*: `testFailedWorklogIsReported`, commit `a897569`).
* **KB-43**: Isolated focus ambient sound generator files in dedicated directories away from shared `/tmp`. (*Test*: `testAmbientSoundIsNotGeneratedInSharedTmp`, commit `a897569`).

---

### KB-44 – KB-56: Bearer Auth, Priority Mappings & Timer Reliability

* **KB-44**: Added Bearer token fallback for Jira worklog submissions upon 401 response. (*Test*: `testJiraWorklogFallsBackToBearerAuth`, commit `82b5093`).
* **KB-45**: Prevented arbitrary text labels from being erroneously parsed as priority markers. (*Test*: `testNonPriorityLabelsDoNotMapToPriority`, commit `82b5093`).
* **KB-46**: Corrected status negation parsing to avoid misclassifying "Not Done" as completed. (*Test*: `testNegatedRemoteStatusesAreNotMisclassified`, commit `82b5093`).
* **KB-47**: Ensured forced Bearer worklog requests are no-ops in test mode. (*Test*: `testForcedBearerWorklogIsNoOpInTestMode`, commit `17b6a8b`).
* **KB-48**: Prevented Bearer mode from becoming sticky after Basic credentials are corrected. (*Test*: `testJiraBearerModeIsNotStickyAfterCredentialFix`, commit `17b6a8b`).
* **KB-49**: Expanded support for common remote priority label formats (e.g., `prio:high`, `p1`). (*Test*: `testCommonPriorityLabelStylesStillMap`, commit `17b6a8b`).
* **KB-50**: Handled project-scoped priority code parsing. (*Test*: `testScopedPriorityCodeLabelsMap`, commit `6f0d732`).
* **KB-51**: Aligned default priority mappings for Todoist and Nextcloud with local values. (*Test*: `testDefaultPriorityMappingMatchesSyncedValues`, commit `6f0d732`).
* **KB-52**: Retried Jira remote priority queries with Bearer authentication on 401. (*Test*: `testJiraFetchRemotePrioritiesRetriesWithBearer`, commit `6f0d732`).
* **KB-53**: Avoided re-binding running timers to mismatched tasks on database backup import. (*Test*: `testImportBackupDoesNotRebindRunningTimerToOtherTask`, commit `6f0d732`).
* **KB-54**: Prevented non-JSON Jira priority responses from wiping existing priority mappings. (*Test*: `testJiraPriorityFetchNonJsonDoesNotWipeMapping`, commit `db35da5`).
* **KB-55**: Completed localization coverage for recently added strings. (*Test*: `testRecentTrStringsAreTranslated`, commit `db35da5`).
* **KB-56**: Ensured manual skip of a Pomodoro work session does not record a completed pomodoro. (*Test*: `testSkippedWorkSessionIsNotACompletedPomodoro`, commit `db35da5`).

---

### KB-57 – KB-69: Nextcloud CalDAV, Pomodoro Continuity & Token Masking

* **KB-57**: Decoded percent-encoded URI sequences when picking files via file dialogs. (*Test*: `testFileDialogPathConversionKeepsReservedCharacters`, commit `a767f7f`).
* **KB-58**: Reloaded the dashboard view immediately upon changing the active list. (*Test*: `testDashboardFollowsCurrentListChange`, commit `a767f7f`).
* **KB-59**: Retained active task and running timer state when re-selecting the current list. (*Test*: `testEditingCurrentListKeepsRunningTimerTask`, commit `598b460`).
* **KB-60**: Re-selecting the current remote list triggers background synchronization without resetting views. (*Test*: `testEditingCurrentRemoteListStillSyncs`, commit `e4ea0a8`).
* **KB-61**: Preserved unclosed code fences in Markdown-to-ADF (Atlassian Document Format) conversion. (*Test*: `testMarkdownToAdfKeepsUnclosedCodeFence`, commit `e4ea0a8`).
* **KB-62**: Eliminated empty text nodes in ADF output that were rejected by Jira Cloud API. (*Test*: `testMarkdownToAdfHasNoEmptyTextNodes`, commit `e4ea0a8`).
* **KB-63**: Preserved unrelated ICS properties during Nextcloud task description updates. (*Test*: `testNextcloudDescriptionUpdateKeepsOtherProperties`, commit `0c3585a`).
* **KB-64**: Confined description and comment updates in Nextcloud strictly to the `VTODO` block, leaving `VALARM` intact. (*Test*: `testNextcloudDescriptionUpdateDoesNotTouchAlarm`, commit `393a146`).
* **KB-65**: Safely updated Nextcloud tasks containing the keyword `BEGIN` in their summary. (*Test*: `testNextcloudDescriptionUpdateWithBeginTextInSummary`, commit `12e796d`).
* **KB-66**: Ensured "All Lists" dashboard mode aggregates metrics across every list. (*Test*: `testDashboardInAllListsViewCoversAllLists`, commit `12e796d`).
* **KB-67**: Preserved Pomodoro cycle count when re-selecting the currently active list. (*Test*: `testReselectingCurrentListKeepsPomodoroCycle`, commit `2ea130c`).
* **KB-68**: Masked API token input in the TUI list editor to prevent credential shoulder-surfing. (*Test*: `testTuiListEditDoesNotPrintApiToken`, commit `05688b9`).
* **KB-69**: Populated `GOFI_REMAINING_SEC` and configured duration in hook execution environments. (*Test*: `testHookEnvironmentMatchesDocumentedVariables`, commit `1e9326e`).

---

### KB-70 – KB-76: TUI Formatting, ANSI Sequences & Dialog Binding

* **KB-70**: Clamped long Markdown headings and tables to terminal width in TUI view. (*Test*: `testTuiMarkdownViewKeepsLongHeadingsInsideTerminalWidth`, commit `3fc5b46`).
* **KB-71**: Bound [TaskDetailsDialog.qml](file:///home/bladeroot/prj/other/Go-For-It/qml/dialogs/TaskDetailsDialog.qml) strictly to task ID instead of dynamic proxy model rows. (*Test*: `testTaskDetailsDialogSavesToTheTaskItWasOpenedFor`, commit `3fc5b46`).
* **KB-72**: Applied shortcut changes immediately without restart via `shortcutMap` binding. (*Test*: `testChangedShortcutAppliesWithoutRestart`, commit `3fc5b46`).
* **KB-73**: Clamped long list names and search filter text in TUI header row. (*Test*: `testTuiHeaderKeepsLongListNameInsideTerminalWidth`, commit `3fc5b46`).
* **KB-74**: Enabled substring search matches for URLs and colon-separated words (`http://`, `bug:`) in [TaskFilterProxyModel.cpp](file:///home/bladeroot/prj/other/Go-For-It/src/core/TaskFilterProxyModel.cpp). (*Test*: `testSearchFindsTasksByUrlAndColonWords`, commit `3fc5b46`).
* **KB-75**: Verified presence of `merged_at` before classifying GitHub pull requests as merged. (*Test*: `testGitHubOpenPrWithoutMergedAtIsNotMerged`, commit `3fc5b46`).
* **KB-76**: Skipped ANSI SGR escape sequences when calculating display width and truncating text in TUI tabs. (*Test*: `testTuiTabRowIsNotTruncatedByAnsiCodes`, commit `7f8b299`).

---

### KB-77 – KB-85: GitHub Queries, TUI Teardown, Heatmap, Redmine, Schedule & TUI Input Clamping

* **KB-77**: Appended `is:open` and `sort:updated-desc` to GitHub repository queries lacking explicit state filters. (*Test*: `testGitHubRepoQueryRequestsOpenItemsOnly`, commit `4691d59`).
* **KB-78**: Recorded and flushed active timer session upon TUI exit (`q`) and application shutdown. (*Test*: `testTuiQuitLogsRunningSession`, commit `4691d59`).
* **KB-79**: Ensured pressing Enter in the TUI list picker selects the highlighted list without stale selections. (*Test*: `testTuiListPickerEnterOpensHighlightedList`, commit `4691d59`).
* **KB-80**: Truncated preview pane rows in TUI to terminal width to prevent layout overflow. (*Test*: `testTuiPreviewPaneKeepsLongTitleInsideTerminalWidth`, commit `4691d59`).
* **KB-81**: Aligned heatmap start dates strictly to Monday, preserved exact counts (14, 28, 91), and localized weekday labels. (*Test*: `testHeatmapColumnsStartOnMonday`, commit `4691d59`).
* **KB-82**: Retained original Redmine closed status names (e.g., `Rejected`) when mapped to category `done`. (*Test*: `testRedmineClosedStatusKeepsNameForMapping`, commit `4691d59`).
* **KB-83**: Handled `INT_MIN` in [Schedule.cpp](file:///home/bladeroot/prj/other/Go-For-It/src/core/Schedule.cpp) to prevent two's-complement overflow and negative durations. (*Test*: `testScheduleNormalizesMinimumIntegerDurations`, commit `dcde392`).
* **KB-84**: Handled GitHub queries containing non-state qualifiers (e.g. `is:issue`) so repository prefixing and default `is:open` filter are not suppressed. (*Test*: `testGitHubQueryWithTypeQualifierStillRequestsOpenItemsOnly`, commit `08673b5`).
* **KB-85**: Clamped parsed minute input in TUI settings before arithmetic conversion to seconds, preventing integer overflow with large inputs. (*Test*: `testTuiLargeDurationInputClampsWithoutOverflow`).

---

## Verification

All listed issues are covered by Qt Test regression tests executed via CTest:

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir /tmp/gofi-qt-build --output-on-failure
```
