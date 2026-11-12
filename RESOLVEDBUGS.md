# Resolved Bugs Registry

This document maintains a comprehensive, structured record of defects, edge cases, and architectural regressions identified and resolved in **Go for It Qt** (`go-for-it-qt`, `go-for-it-tui`).

All entries are validated by regression tests in [tests/tst_todotxt.cpp](tests/tst_todotxt.cpp).

---

## Table of Contents

1. [Part 1: Foundational Defects & Architectural Hardening (Pre-KB)](#part-1-foundational-defects--architectural-hardening-pre-kb)
   - [1.1. SQLite Persistence, Transaction Atomicity & Reordering](#11-sqlite-persistence-transaction-atomicity--reordering)
   - [1.2. Timer Integrity, Activity Logging & Anti-Leakage](#12-timer-integrity-activity-logging--anti-leakage)
   - [1.3. Remote Issue Tracker Synchronization](#13-remote-issue-tracker-synchronization)
   - [1.4. Test Environment Isolation & System Safety](#14-test-environment-isolation--system-safety)
   - [1.5. User Interface (GUI / QML / TUI)](#15-user-interface-gui--qml--tui)
   - [1.6. Packaging & Linux Distribution Compatibility](#16-packaging--linux-distribution-compatibility)
2. [Part 2: Systematic Review Defects (KB-1 through KB-89)](#part-2-systematic-review-defects-kb-1-through-kb-89)
   - [KB-1 – KB-11: First Review Round (Status Mapping, Pagination, Timer, Translations)](#kb-1--kb-11-first-review-round-status-mapping-pagination-timer-translations)
   - [KB-12 – KB-26: Reconciliation Safety, Pagination Shifts, Onboarding, URL Schemes & Relocation](#kb-12--kb-26-reconciliation-safety-pagination-shifts-onboarding-url-schemes--relocation)
   - [KB-27 – KB-43: List Mutation Safety, Overflow, Permissions, Export Failures & TUI Input](#kb-27--kb-43-list-mutation-safety-overflow-permissions-export-failures--tui-input)
   - [KB-44 – KB-56: Bearer Auth, Priority Mappings & Timer Reliability](#kb-44--kb-56-bearer-auth-priority-mappings--timer-reliability)
   - [KB-57 – KB-69: Nextcloud CalDAV, Pomodoro Continuity & Token Masking](#kb-57--kb-69-nextcloud-caldav-pomodoro-continuity--token-masking)
   - [KB-70 – KB-76: TUI Formatting, ANSI Sequences & Dialog Binding](#kb-70--kb-76-tui-formatting-ansi-sequences--dialog-binding)
   - [KB-77 – KB-89: GitHub Queries, TUI Teardown, Heatmap, Remote Status Sync & Input Validation](#kb-77--kb-89-github-queries-tui-teardown-heatmap-remote-status-sync--input-validation)

---

## Part 1: Foundational Defects & Architectural Hardening (Pre-KB)

### 1.1. SQLite Persistence, Transaction Atomicity & Reordering

* **Zero Affected Rows Guard**:
  * *Problem*: In batch reordering and moving completed tasks, a failure on a single row previously left database state silently desynchronized from the in-memory model.
  * *Fix*: Enforced strict verification of `rowsAffected > 0` on every batch update step, rolling back (`ROLLBACK`) on zero affected rows.
  * *Commits*: `a962389`, `458f702`, `77044f1`.
  * *Test*: `testAtomicDoneAndWorklogSyncAndOrderCompaction`.

* **Sort Order Compaction on Task Deletion**:
  * *Problem*: Deleting tasks left gaps in `sort_order`, resulting in integer exhaustion and erratic ordering on subsequent prepend/insert operations.
  * *Fix*: Implemented atomic compaction of `sort_order` scoped to the current list on task removal.
  * *Commits*: `5f87bc7`, `10c146e`, `a742564`.

* **Transactional Savepoints (`SAVEPOINT`) and Model Rollback**:
  * *Problem*: When SQLite writes failed, the in-memory Qt models had already updated their internal vectors, causing UI and disk state to diverge.
  * *Fix*: Introduced transactional savepoints in [DatabaseManager.cpp](src/core/DatabaseManager.cpp). Model vectors are updated only via callbacks after database commit confirmation, rolling back on failure.
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
  * *Fix*: Bound timer state strictly to `taskId` ([AppController.cpp](src/core/AppController.cpp)).
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
  * *Fix*: Implemented recursive asynchronous pagination with accumulated results in [IssueTrackerService.cpp](src/core/IssueTrackerService.cpp).
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
  * *Fix*: Declared property explicitly in [TxtListEditDialog.qml](qml/dialogs/TxtListEditDialog.qml).
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

## Part 2: Systematic Review Defects (KB-1 through KB-89)

### KB-1 – KB-11: First Review Round (Status Mapping, Pagination, Timer, Translations)

All fixed in commit `96dede1` unless noted.

* **KB-1**: Learning Redmine status ids during a sync emitted `listUpdated`, which re-selected the list, stopped the running timer and left the multi-list view. (*Test*: `testRedmineStatusMappingDoesNotResetTimerOrMultiList`).
* **KB-2**: Learned Redmine status ids were mostly ineffective and got lost when the list dialog saved the mapping.
* **KB-3**: Offset pagination combined with reconciliation could close a still-open task.
* **KB-4**: GitLab treated a non-JSON `200` response as a successful (empty) result.
* **KB-5**: YouTrack pagination counted parsed items instead of raw items when deciding whether more pages exist.
* **KB-6**: Reconciliation never ran in some valid cases (explicitly empty results).
* **KB-7**: The default GitLab query could fetch thousands of foreign items (scope narrowed to the user's own items).
* **KB-8**: The timer double-counted sub-second remainders after a flush. (*Test*: `testTaskTimerNoDoubleCountingOnFlush`).
* **KB-9**: Translation gaps and leftover keys in the language files.
* **KB-10**: Tests used fixed `/tmp` paths instead of the test sandbox.
* **KB-11**: Not a defect: "local done is reverted on the next sync when status push is off" was recorded as a product question and has no fix.

---

### KB-12 – KB-26: Reconciliation Safety, Pagination Shifts, Onboarding, URL Schemes & Relocation

* **KB-12**: Nextcloud — a 2xx CalDAV answer that is not a multistatus with VTODOs (e.g. an HTML login page after a redirect) is no longer treated as "explicitly empty". (*Test*: `testNextcloudNonCalDavBodyDoesNotCloseTasks`, commit `fb7e847`).
* **KB-13**: Redmine — offset pagination that skips an item when an earlier item leaves the result set no longer closes the skipped, still-open issue. (*Test*: `testRedminePaginationShiftDoesNotCloseSkippedIssue`, commit `fb7e847`).
* **KB-14**: Redmine — the learned `is_closed` fallback skips rejected/canceled statuses, like the explicit-mapping path. (*Test*: `testRedmineLearnedClosedFallbackSkipsRejected`, commit `fb7e847`).
* **KB-15**: GitLab — the KB-13 pagination-shift guard was extended to GitLab (`page` + `X-Next-Page`). (*Test*: `testGitLabPaginationShiftDoesNotCloseSkippedIssue`, commit `5edf4cc`).
* **KB-16**: Nextcloud — a 207 multistatus in which every object reports `calendar-data` as 404/403 no longer closes local tasks. (*Test*: `testNextcloudMultistatusWithoutCalendarDataDoesNotCloseTasks`, commit `5edf4cc`).
* **KB-17**: Nextcloud — when only some objects return `calendar-data`, the objects without data are not reconciled as closed. (*Test*: `testNextcloudPartialCalendarDataDoesNotCloseMissingTasks`, commit `c77d5b7`).
* **KB-18**: YouTrack — the KB-15 fix forced `limitReached` whenever more than one page was needed, so lists with 100 or more open issues never reconciled. (*Test*: `testYouTrackMultiPageListStillReconcilesClosedIssue`, commit `c77d5b7`).
* **KB-19**: YouTrack — KB-18 was fixed only for exactly 100 issues; lists with two non-empty pages still never reconciled. (*Test*: `testYouTrackLargeStableListStillReconcilesClosedIssue`, commit `b280825`).
* **KB-20**: YouTrack — the KB-19 boundary heuristic treated a remotely closed issue sitting exactly at a page boundary as a pagination shift, so it was never closed locally. (*Test*: `testYouTrackClosedIssueAtPageBoundaryIsReconciled`, commit `fb43f6c`).
* **KB-21**: YouTrack — the KB-19 checks missed a shift starting on page 2 when the page 2/3 boundary crosses projects; replaced by a two-pass check. (*Test*: `testYouTrackCrossProjectShiftOnLaterPageDoesNotCloseSkippedIssue`, commit `fb43f6c`).
* **KB-22**: YouTrack — removed a production special case for the test list id `kb15_yt`; identical server responses now give the same result for any list id. (*Tests*: `testYouTrackSyncOutcomeDoesNotDependOnListId`, `testYouTrackTwoPassDetectsRealisticShift`, commit `24c0fd2`).
* **KB-23**: The three onboarding tasks were re-inserted every time a local list with zero tasks was selected; they are now added only once per list. (*Test*: `testDefaultTasksNotReaddedAfterUserEmptiesList`, commit `f7b3d91`).
* **KB-24**: Links from tracker-controlled text (Markdown details/comments) bypassed the `http`/`https`/`mailto` whitelist; `AppController::openUrlExternally` now rejects other schemes. (*Test*: `testOpenUrlExternallyRejectsUnsafeSchemes`, commit `f7b3d91`).
* **KB-25**: A failed database relocation no longer leaves the app without a database or persists the bad path to `settings.ini`. (*Test*: `testFailedDatabaseRelocationKeepsCurrentDatabase`, commit `f7b3d91`).
* **KB-26**: Test guard — a relaxed Jira auth test no longer caught a retry loop; when both Basic and Bearer are rejected, exactly one attempt of each is now required. (*Test*: `testJiraAuthRetryIsBoundedWhenBothSchemesFail`, commits `7c606aa`, `1dc86a4`).

---

### KB-27 – KB-43: List Mutation Safety, Overflow, Permissions, Export Failures & TUI Input

* **KB-27**: Mutating a list row that no longer exists is no longer reported as a successful persistence operation. (*Test*: `testListMutationsRejectMissingRows`, commit `c816a12`).
* **KB-28**: A list already deleted elsewhere is dropped from the UI model instead of staying in memory. (*Test*: `testRemovingListAlreadyDeletedElsewhereDropsItFromModel`, commit `360a0f6`).
* **KB-29**: Duration/timer components that do not fit in `quint32` seconds stay ordinary text instead of wrapping. (*Test*: `testDurationTokensRejectArithmeticOverflow`, commit `6e2922d`).
* **KB-30**: Billing exports are created owner-only (`0600`). (*Test*: `testBillingExportFilesAreOwnerOnly`, commit `27dc901`).
* **KB-31**: `exportBillingPdf` reports failure when the target cannot be written instead of returning success. (*Test*: `testBillingPdfExportReportsFailureWhenTargetCannotBeWritten`, commit `07d9771`).
* **KB-32**: Removing `due:YYYY-MM-DD` from a task's text now clears its due date. (*Test*: `testEditingTextRemovesDueDate`, commit `79cc64d`).
* **KB-33**: Notification bodies are escaped before being sent to `org.freedesktop.Notifications`. (*Test*: `testNotificationPayloadEscapesMarkup`, commit `79cc64d`).
* **KB-34**: The per-list activity log CSV is owner-only (`0600`). (*Test*: `testActivityLogCsvIsOwnerOnly`, commit `79cc64d`).
* **KB-35**: Notification titles are plain text per the spec and are no longer HTML-escaped. (*Test*: `testNotificationTitleIsNotHtmlEscaped`, commit `79d5091`).
* **KB-36**: The billing task breakdown keeps tasks of different lists apart. (*Test*: `testBillingBreakdownKeepsTasksOfDifferentListsApart`, commit `79d5091`).
* **KB-37**: Every remote tracker write path is a no-op in test mode. (*Test*: `testTrackerWritesAreNoOpsInTestMode`, commit `79d5091`).
* **KB-38**: TUI title edits keep the `(A)` priority and `duration:` tokens. (*Test*: `testTuiStyleTitleEditKeepsPriorityAndDuration`, commit `79d5091`).
* **KB-39**: TUI input keeps multibyte UTF-8 split across reads and non-BMP characters (emoji). (*Test*: `testTuiInputKeepsMultibyteAndNonBmpCharacters`, commit `79d5091`).
* **KB-40**: TUI input splits escape sequences from typed text in the same read. (*Test*: `testTuiSplitsEscapeSequencesFromTypedText`, commit `eb30c2c`).
* **KB-41**: The remote priority "Lowest" maps to `(E)` instead of `(D)`. (*Test*: `testLowestRemotePriorityMapsToE`, commit `eb30c2c`).
* **KB-42**: Remote worklog failures (401, server errors, unreachable) are reported. (*Test*: `testFailedWorklogIsReported`, commit `a897569`).
* **KB-43**: Ambient sound files are generated in the private cache directory instead of a predictable name in `/tmp`. (*Test*: `testAmbientSoundIsNotGeneratedInSharedTmp`, commit `a897569`).

---

### KB-44 – KB-56: Bearer Auth, Priority Mappings & Timer Reliability

* **KB-44**: Added Bearer token fallback for Jira worklog submissions upon 401 response. (*Test*: `testJiraWorklogFallsBackToBearerAuth`, commit `82b5093`).
* **KB-45**: Prevented arbitrary text labels from being erroneously parsed as priority markers. (*Test*: `testNonPriorityLabelsDoNotMapToPriority`, commit `82b5093`).
* **KB-46**: Corrected status negation parsing to avoid misclassifying "Not Done" as completed. (*Test*: `testNegatedRemoteStatusesAreNotMisclassified`, commit `82b5093`).
* **KB-47**: Ensured forced Bearer worklog requests are no-ops in test mode. (*Test*: `testForcedBearerWorklogIsNoOpInTestMode`, commit `17b6a8b`).
* **KB-48**: Prevented Bearer mode from becoming sticky after Basic credentials are corrected. (*Test*: `testJiraBearerModeIsNotStickyAfterCredentialFix`, commit `17b6a8b`).
* **KB-49**: Restored mapping of common GitHub/GitLab label styles that the KB-45 change broke (`priority-high`, `priority_high`, `high-priority`, `P1 - High`, `prio-low`). (*Test*: `testCommonPriorityLabelStylesStillMap`, commit `17b6a8b`).
* **KB-50**: Restored `P0`–`P4` codes after scoped prefixes such as GitLab `priority::P1`, `priority/p2`, `prio:P0`. (*Test*: `testScopedPriorityCodeLabelsMap`, commit `6f0d732`).
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
* **KB-71**: Bound [TaskDetailsDialog.qml](qml/dialogs/TaskDetailsDialog.qml) strictly to task ID instead of dynamic proxy model rows. (*Test*: `testTaskDetailsDialogSavesToTheTaskItWasOpenedFor`, commit `3fc5b46`).
* **KB-72**: Applied shortcut changes immediately without restart via `shortcutMap` binding. (*Test*: `testChangedShortcutAppliesWithoutRestart`, commit `3fc5b46`).
* **KB-73**: Clamped long list names and search filter text in TUI header row. (*Test*: `testTuiHeaderKeepsLongListNameInsideTerminalWidth`, commit `3fc5b46`).
* **KB-74**: Enabled substring search matches for URLs and colon-separated words (`http://`, `bug:`) in [TaskFilterProxyModel.cpp](src/todotxt/TaskFilterProxyModel.cpp). (*Test*: `testSearchFindsTasksByUrlAndColonWords`, commit `3fc5b46`).
* **KB-75**: Verified presence of `merged_at` before classifying GitHub pull requests as merged. (*Test*: `testGitHubOpenPrWithoutMergedAtIsNotMerged`, commit `3fc5b46`).
* **KB-76**: Skipped ANSI SGR escape sequences when calculating display width and truncating text in TUI tabs. (*Test*: `testTuiTabRowIsNotTruncatedByAnsiCodes`, commit `7f8b299`).

---

### KB-77 – KB-89: GitHub Queries, TUI Teardown, Heatmap, Remote Status Sync & Input Validation

* **KB-77**: Appended `is:open` and `sort:updated-desc` to GitHub repository queries lacking explicit state filters. (*Test*: `testGitHubRepoQueryRequestsOpenItemsOnly`, commit `4691d59`).
* **KB-78**: Recorded and flushed active timer session upon TUI exit (`q`) and application shutdown. (*Test*: `testTuiQuitLogsRunningSession`, commit `4691d59`).
* **KB-79**: Ensured pressing Enter in the TUI list picker selects the highlighted list without stale selections. (*Test*: `testTuiListPickerEnterOpensHighlightedList`, commit `4691d59`).
* **KB-80**: Truncated preview pane rows in TUI to terminal width to prevent layout overflow. (*Test*: `testTuiPreviewPaneKeepsLongTitleInsideTerminalWidth`, commit `4691d59`).
* **KB-81**: Aligned heatmap start dates strictly to Monday, preserved exact counts (14, 28, 91), and localized weekday labels. (*Test*: `testHeatmapColumnsStartOnMonday`, commit `4691d59`).
* **KB-82**: Retained original Redmine closed status names (e.g., `Rejected`) when mapped to category `done`. (*Test*: `testRedmineClosedStatusKeepsNameForMapping`, commit `4691d59`).
* **KB-83**: Handled `INT_MIN` in [Schedule.cpp](src/core/Schedule.cpp) to prevent two's-complement overflow and negative durations. (*Test*: `testScheduleNormalizesMinimumIntegerDurations`, commit `dcde392`).
* **KB-84**: Handled GitHub queries containing non-state qualifiers (e.g. `is:issue`) so repository prefixing and default `is:open` filter are not suppressed. (*Test*: `testGitHubQueryWithTypeQualifierStillRequestsOpenItemsOnly`, commit `08673b5`).
* **KB-85**: Clamped parsed minute input in TUI settings before arithmetic conversion to seconds, preventing integer overflow with large inputs. (*Test*: `testTuiLargeDurationInputClampsWithoutOverflow`, commit `f1d8af6`).
* **KB-86**: Implemented remote issue completion and reopening status sync for Jira (via transitions API), YouTrack (via commands API), and Nextcloud CalDAV (via VTODO STATUS). (*Test*: `testRemoteStatusSyncDispatchesForJiraYouTrackAndNextcloud`, commit `d40b2bc`).
* **KB-87**: Included all supported remote providers (Redmine, Todoist, Nextcloud) in `AppController::isCurrentListRemote()` single-list classification. (*Test*: `testCurrentListRemoteFlagIncludesAllRemoteProviders`, commit `d40b2bc`).
* **KB-88**: Rejected empty or non-numeric TUI duration, reminder and auto-sync input (e.g. `25m`) instead of falling back to the maximum value; the current value is kept and an error is shown in the status line. Digits-only input that overflows is still clamped. (*Test*: `testTuiEmptyDurationInputDoesNotSetMaximum`).
* **KB-89**: YouTrack status push now picks the state mapped to `done` on completion and the state mapped to `todo` (then `in_progress`) on reopen, skips `_` metadata keys, and wraps state names in braces (`State {In Review}`). (*Test*: `testYouTrackReopenUsesTodoStateAndBracesMultiWordNames`).
* **KB-86 follow-up**: Reworked the status push added for KB-86. Jira reopen now targets the "To Do" status category (or "In Progress" for a local `in_progress` status) instead of the first "new"/"indeterminate" transition, retries with Bearer auth on 401/403 (and remembers Bearer mode only after success), and reports a missing matching transition through `syncFailed` instead of failing silently. Nextcloud completion now writes `STATUS:COMPLETED`, `COMPLETED:<UTC>` and `PERCENT-COMPLETE:100`; reopening writes `STATUS:NEEDS-ACTION` and removes the two completion properties, all inside the VTODO property block only. (*Tests*: `testJiraReopenPrefersToDoTransition`, `testJiraStatusPushFallsBackToBearerAuth`, `testJiraStatusPushReportsMissingTransition`, `testNextcloudStatusPushKeepsCompletionPropertiesConsistent`).

---

### User-reported defects

* **KB-90 — GitLab sync always failed with HTTP 400**: since `96dede1` every GitLab request (project, `group:` and assigned-to-me queries, issues and merge requests) was sent with `order_by=id`, which the GitLab REST API rejects with `400 {"error":"order_by does not have a valid value"}`, so GitLab sync failed whatever the server URL and token. Requests now use `order_by=created_at&sort=asc` (immutable, so offset pagination stays stable for the KB-15 shift guard), and the sync error includes the `error`/`message` text from GitLab's response body instead of only "Bad Request". (*Tests*: `testGitLabSyncUsesOrderByAcceptedByGitLab`, `testGitLabSyncErrorShowsServerMessage`).
* **Mini player closed with the main window**: `MiniPlayerWindow` is declared inside the main `ApplicationWindow`, so Qt made the main window its transient parent and hid the mini player whenever the main window was hidden to the tray. The mini player is now an independent top-level window (`transientParent: null`), and closing the main window while the mini player is open hides it instead of closing it. (*Test*: `testMiniPlayerSurvivesHidingMainWindow`).
* **Mini player could not be dragged**: the drag area added the pointer delta to `window.x/y` while the window moved under the pointer (jitter on X11, no effect on Wayland). Dragging now calls `Window.startSystemMove()` and falls back to manual positioning with global coordinates only when that is unsupported. (*Test*: `testMiniPlayerDragUsesSystemMove`, a source-level check because window-system moves cannot be exercised offscreen).

---

## Verification

All listed issues are covered by Qt Test regression tests executed via CTest:

```bash
build_dir=$(mktemp -d /tmp/go-for-it-build.XXXXXX)
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$build_dir" -j2
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" --output-on-failure
cmake -E remove_directory "$build_dir"
```
