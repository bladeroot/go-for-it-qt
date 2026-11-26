# Resolved Bugs Registry

This document maintains a comprehensive, structured record of defects, edge cases, and architectural regressions identified and resolved in **Go for It Qt** (`go-for-it-qt`, `go-for-it-tui`).

All entries are validated by regression tests in the `tests/tst_*.cpp` suites (`tst_core`, `tst_trackers`, `tst_tui`, `tst_qml`, `tst_credentials`; before the split they all lived in `tests/tst_todotxt.cpp`).

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

* **KB-90 — GitLab sync always failed with HTTP 400**: since `96dede1` every GitLab request (project, `group:` and assigned-to-me queries, issues and merge requests) was sent with `order_by=id`, which the GitLab REST API rejects with `400 {"error":"order_by does not have a valid value"}`, so GitLab sync failed whatever the server URL and token. Requests now use `order_by=created_at&sort=asc` (immutable, so offset pagination stays stable for the KB-15 shift guard), and the sync error includes the `error`/`message` text from GitLab's response body instead of only "Bad Request". (*Tests*: `testGitLabSyncUsesOrderByAcceptedByGitLab`, `testGitLabSyncErrorShowsServerMessage`, commit `0b764c0`).
* **Mini player closed with the main window**: `MiniPlayerWindow` is declared inside the main `ApplicationWindow`, so Qt made the main window its transient parent and hid the mini player whenever the main window was hidden to the tray. The mini player is now an independent top-level window (`transientParent: null`), and closing the main window while the mini player is open hides it instead of closing it. (*Test*: `testMiniPlayerSurvivesHidingMainWindow`, commit `8c40b66`).
* **Mini player could not be dragged**: the drag area added the pointer delta to `window.x/y` while the window moved under the pointer (jitter on X11, no effect on Wayland). Dragging now calls `Window.startSystemMove()` and falls back to manual positioning with global coordinates only when that is unsupported. (*Test*: `testMiniPlayerDragUsesSystemMove`, a source-level check because window-system moves cannot be exercised offscreen, commit `8c40b66`).
* **Opening task details required hunting for the row icon button**: double-clicking a task row in `TaskRowDelegate` previously entered inline title editing, pressing `Enter`/`Return` on a selected task did nothing, and the active task in `TimerView` had no way to open its description and comments. Double-clicking or pressing `Enter`/`Return` now opens `TaskDetailsDialog` both in `TaskListView` and for the active task in `TimerView` (inline title editing remains on the `✎` button). (*Test*: `testMainWindowOpensTaskDetailsOnDoubleClickAndEnter`, commit `d92a8a6`).
* **Mini player hid on window switch, had a jumping Play/Pause button, lacked Always-on-Top / tray / ambient controls, and ambient sound names were invisible in TimerView**: `TimerView.qml` bound `textRole: "label"` on `ambientCombo` while `AmbientSoundService::availableModes()` only returned `"name"`, leaving dropdown rows blank; `MiniPlayerWindow` set `Qt.Tool` (causing window managers to hide it when switching windows), switched Play/Pause between `"▶"` and emoji `"⏸"` with dynamic neighbor visibility (causing the button to jump), and had no way to toggle Always on Top, switch ambient sound, or toggle the mini player from the system tray menu. Fixed all four issues. (*Test*: `testMiniPlayerAlwaysOnTopAmbientSoundAndStablePlayPause`, commit `df389df`).
* **KB-91 — Enter in text fields or spin boxes opened the task details dialog**: `TextInput` handled Return (emitted `accepted`) and then ignored the key event, propagating it to parent `Keys.onReturnPressed` handlers in `TaskListPage` and `TimerView`. Fields now explicitly accept the event and `TaskListPage`/`TimerView` guard against active editor focus. (*Test*: `testReturnInEditorsDoesNotOpenTaskDetails`, commit `86f2dfc`).
* **KB-92 — Enter in To-Do list opened `ListView.currentIndex` instead of active task**: `currentIndex` only tracked mouse clicks and did not update when switching tasks via shortcuts, tray or mini player. `tasksView.currentIndex` is now kept synchronized with `app.activeTodoRow` and `openSelectedTaskDetails()` prioritizes the active task row. (*Test*: `testReturnInTodoListOpensHighlightedActiveTask`, commit `86f2dfc`).
* **KB-93 — Ambiguous duplicate Ctrl+M shortcut broke mini player toggle**: `Main.qml` declared `Shortcut { sequence: "Ctrl+M" }` twice in the same window context, emitting `activatedAmbiguously` instead of `activated`. Removed the redundant shortcut declaration. (*Test*: `testCtrlMTogglesMiniPlayer`, commit `86f2dfc`).
* **KB-94 — Untranslated user-visible strings**: 7 strings added since 2.0.1 (`Unknown ambient mode. Available: none, white, rain, ocean, clock`, `Double-click or press Enter to view description & comments`, `No matching Jira transition to update status of %1; status was not changed on %2.`, `Invalid number: "%1" (value unchanged)`, `Mon`, `Wed`, `Fri`) had no keys in the translation dictionaries (`uk`, `de`, `fr`, `es`, `it`), so they were shown in English. Added the keys to all five files; the files were also re-sorted by key, which is cosmetic (`QJsonObject` keeps its keys sorted itself, so file order never affected lookup). (*Test*: `testStringsAddedSince201AreTranslated`, commit `86f2dfc`).
* **KB-95 — Dashboard analytics test failures between 00:00 and 01:02 local time**: `testDashboardFollowsCurrentListChange` and `testDashboardInAllListsViewCoversAllLists` inserted sessions starting at `now - 3700s` and queried the default `"day"` period, which rolled over into the previous calendar day in the first hour after midnight. Tests now anchor timestamps deterministically at local noon. (*Tests*: `testDashboardFollowsCurrentListChange`, `testDashboardInAllListsViewCoversAllLists`, commit `86f2dfc`).
* **KB-96 — Mini player disappeared or fell behind other windows on Wayland (KWin / KDE Plasma)**: Standard `xdg-shell` protocol ignores client `WindowStaysOnTopHint`, and QML parent hierarchy bound `MiniPlayerWindow` as a transient child of `ApplicationWindow`. The window is now detached as an independent top-level window (`setParent(nullptr)`, `setTransientParent(nullptr)`) and enforced on top and across all desktops via KWin Scripting D-Bus interface with retry timers for asynchronous Wayland surface mapping. (*Tests*: `testMiniPlayerDetachAndAlwaysOnTopConfiguration`, `testMiniPlayerAlwaysOnTopAmbientSoundAndStablePlayPause`, commits `db6c0ce`, `38f2dae`). Note: these tests do not guard the fix: `testMiniPlayerDetachAndAlwaysOnTopConfiguration` also passes with the pre-fix QML (`86f2dfc` already had `transientParent: null` and `WindowStaysOnTopHint`), and the KWin part is skipped in test mode; the pin script text is covered by `testKWinPinScriptTouchesOnlyOwnMiniPlayerWindow` (KB-97).
* **KB-97 — KWin pin script overmatched foreign application windows**: The initial KWin pin script matched windows by substring caption or low height, which could modify foreign windows (VLC, Spotify, browser tabs) or the main window when resized. The script now strictly matches the process's own PID (`w.pid === applicationPid()`) and exact caption, leaving foreign windows and taskbar/pager flags untouched. (*Test*: `testKWinPinScriptTouchesOnlyOwnMiniPlayerWindow`, commit `c8a81ff`).
* **KB-98 — Nextcloud status push errors lost without notification on network error or missing VTODO**: In `IssueTrackerService::updateIssueStatusRemote`, the finished callback for the initial GET request previously passed the already-finished reply to `handleReply()`, whose `finished` connection never fired; invalid responses without a VTODO block also returned silently. `failWithError` is now invoked immediately on GET network/HTTP failure or missing VTODO, emitting `syncFailed` and setting the status bar message. (*Test*: `testNextcloudStatusPushEmitsSyncFailedOnErrorAndInvalidVTodo`, commit `177089d`).
* **KB-99 — `getActivityHeatmap()` returned incorrect date range (future days or truncated recent days)**: `AnalyticsService::getActivityHeatmap` aligned the starting date to Monday but kept an iteration limit of `numDays`, resulting in truncated recent days when `daysBack` was not a multiple of 7 (e.g. 365 days ended days before today) or produced future days up to Sunday when it was a multiple of 7. The range now strictly spans from the Monday of the nominal start week up to and including `today` (`startDate.daysTo(today) + 1`). (*Test*: `testActivityHeatmapRangeAndBoundaryAlignment`, commit `177089d`).
* **KB-100 — Synchronous KWin D-Bus calls in `DesktopIntegration::setKWinWindowProperties` could block GUI**: `executeKWinPinScript` previously performed three blocking `bus.call()` operations per invocation with default timeouts, freezing the UI thread if KWin Scripting was slow or deadlocked. Converted to asynchronous `bus.asyncCall()` with 500 ms timeouts and `QDBusPendingCallWatcher` chaining. (*Test*: `testKWinScriptFileStaysReadableUntilRunAndNamesAreUnique`, commits `177089d`, `9607819`; see follow-up fix in KB-102).
* **KB-101 — Test suite manually mutated global test mode without RAII guards**: Several network integration tests manually disabled `QStandardPaths::setTestModeEnabled(false)` and only re-enabled it after assertions, risking leaving test mode disabled for the rest of the test suite if an assertion failed. Replaced all manual test mode manipulations across `tests/tst_todotxt.cpp` with the RAII `RemoteWritesEnabled` guard. (Test infrastructure only, commit `177089d`).
* **KB-102 — KWin script file deleted before asynchronous evaluation and plugin name collision**: `DesktopIntegration::executeKWinPinScript` captured `tempFile` only in the `loadScript` reply callback, allowing `QTemporaryFile` to be deleted before KWin's asynchronous `Script::run()` loaded it from disk. In addition, identical timestamps within the same millisecond caused plugin name collisions and script unloads. Fixed by keeping `tempFile` alive in both `load` and `run` callbacks until `run()` completes, adding an atomic sequence counter for guaranteed uniqueness, and verifying `reply.value() >= 0`. (*Test*: `testKWinScriptFileStaysReadableUntilRunAndNamesAreUnique`, commit `9607819`).
* **KB-103 — TUI dashboard project and context lines garbled by printf-style format**: In `TuiApp::renderDashboardView`, format strings used `+%-16s` and `@%-16s`, which `QString::arg` does not recognize, printing `%-16s` literally and misplacing argument substitutions. Fixed by padding names with `QString::leftJustified(16, ' ')` and formatting with multi-argument `.arg()`. (*Test*: `testTuiDashboardProjectAndContextLinesAreFormatted`, commit `9607819`).
* **KB-104 — Untranslated CalDAV VTODO response error and synthetic HTTP code**: Added missing translation key `"Invalid CalDAV VTODO response"` across all translation dictionaries (`uk`, `de`, `fr`, `es`, `it`), and replaced synthetic status code `422` with the actual HTTP status code returned by the server. (*Test*: `testStringsAddedSince201AreTranslated`, commit `9607819`).
* **KB-105 — Plaintext remote list authentication tokens stored in SQLite database files and backups**: API tokens and personal access tokens (PATs) for remote issue trackers (Jira, GitHub, GitLab, Redmine, Todoist, Nextcloud CalDAV) were previously stored as plaintext strings in the `lists.api_token` SQLite column, causing credentials to be exposed in unencrypted database files and backups (`exportDatabaseBackup`). Introduced `CredentialStore` with primary secure storage via the Freedesktop Secret Service API (`org.freedesktop.secrets` via D-Bus) and an atomic fallback file (`credentials.json` with strict `0600` file permissions) when a keyring daemon is absent. Added automatic transparent migration from legacy plaintext SQLite rows on startup, ensuring `lists.api_token` in SQLite and database backups remains strictly empty. (*Tests*: `testCredentialStoreFallbackFileOperationsAndPermissions`, `testDatabasePlaintextTokenMigrationAndStorageSafety`, `testCredentialStoreKeyringAndMockSecretService`, commits `75db016`, `90d6934`, `217dc9e`).
* **KB-106 — Test suite accessed user's real keyring on the session bus**: `CredentialStore::isKeyringAvailable()` queried `QDBusConnection::sessionBus()` even when running in Qt test mode, reading and writing test tokens directly into the user's active wallet (KWallet / GNOME Keyring) and breaking fallback file tests. Fixed by ensuring `isKeyringAvailable()` returns `false` when `QStandardPaths::isTestModeEnabled()` is active unless an explicit test D-Bus connection is configured. (*Test*: `testCredentialStoreDoesNotUseSessionKeyringInTestMode`, commit `a67fbc6`).
* **KB-107 — Tokens in locked keyring collections were ignored**: `CredentialStore::loadFromKeyring` and `deleteFromKeyring` previously checked only the `unlocked` item list returned by `org.freedesktop.Secret.Service.SearchItems`. When a collection/wallet was locked, tokens could not be read, causing remote syncs to fail with auth errors and risking token deletion on save. Fixed by detecting `locked` item paths and invoking `org.freedesktop.Secret.Service.Unlock` before retrieving or deleting secrets. (*Test*: `testCredentialStoreUnlocksLockedKeyringItems`, commit `a67fbc6`; prompt handling follow-up in KB-110).
* **KB-108 — Remote credentials were not scoped to database instances**: Tokens were stored and indexed solely by `list_id`, causing copies of a database (such as `--db` test copies, imported backups, or relocated databases) sharing list IDs to overwrite or delete credentials from the original database in both the keyring and the directory's `credentials.json`. Fixed by scoping keyring attributes and fallback file keys with `db_path`, and safely merging tokens in `DatabaseManager::relocateDatabase`. (*Test*: `testRemovingListInDatabaseCopyKeepsOriginalToken`, commit `a67fbc6`).
* **KB-109 — Fallback credential file created with process umask and non-atomic replacement**: `CredentialStore::writeFallbackJson` created temporary files with default umask `0644` before applying `0600`, removed the old file prior to `rename()`, and used predictable temporary file names in `/tmp`. Converted to `QSaveFile` with atomic commit. (*Commit*: `a67fbc6`; initial permissions window follow-up in KB-112).
* **KB-110 — Locked keyring collections requiring prompt were not unlocked**: In real Secret Service daemons (KWallet's `ksecretd`, GNOME Keyring), unlocking a locked collection returns an empty unlocked list and a prompt object path (`org.freedesktop.Secret.Prompt`). `CredentialStore` previously ignored the prompt path, leaving tokens unread and returning empty strings. Fixed by invoking `org.freedesktop.Secret.Prompt.Prompt` and listening for the `Completed` signal to retrieve the unlocked item paths before reading or deleting secrets. (*Test*: `testCredentialStoreCompletesUnlockPromptForLockedItems`, commit `a2f10e9`).
* **KB-111 — Unscoped fallbacks matched foreign database tokens and prevented token clearance**: `loadFromKeyring` previously fell back to searching with only `application` and `list_id` if scoped search was empty, matching items belonging to other databases with the same list ID and causing database copies to resurrect original tokens after clearing. Furthermore, `deleteFromFile` did not clean up legacy unscoped keys in `credentials.json`, leaving plaintext tokens behind after clearing or keyring migration. Fixed by restricting keyring search strictly to the current database scope (`db_path`), removing unscoped keys on delete and token save, and ensuring migrated file tokens are purged. (*Tests*: `testClearedTokenStaysClearedDespiteUnscopedEntries`, `testUnscopedFallbackTokenLeavesFileAfterKeyringMigration`, commit `a2f10e9`).
* **KB-112 — Temporary credential fallback file exposed with default umask before commit**: `QSaveFile::fileName()` returns the final destination path rather than the temporary file name. For a newly created `credentials.json`, setting permissions on `file.fileName()` before `commit()` failed to restrict the temporary file, creating it with process umask `0644` and leaving a window where group/other users could read credentials. Fixed by wrapping directory and `QSaveFile` creation in an RAII `UmaskGuard(0077)` in `writeFallbackJson` and `relocateDatabase`, guaranteeing owner-only `0600`/`0700` modes at the kernel syscall level upon creation. (*Commit*: `a2f10e9`).
* **KB-113 — Keyring unlock prompt had restrictive 5-second timeout and per-list UI freezes**: `CredentialStore::unlockObjects` waited only 5 seconds for user password input, aborting the prompt while the master password dialog was still open, returning empty tokens, and causing subsequent settings saves to delete remote credentials. Furthermore, sequential per-list unlock calls caused repeated dialog prompts and froze the UI thread for up to 10 seconds per list. Fixed by batch-unlocking all locked items for the database scope upfront in `DatabaseManager::loadLists` via `CredentialStore::unlockKeyringForCurrentDatabase`, increasing prompt response timeout to human-scale interaction time (120 s), and remembering dismissed prompts to prevent cascading dialog prompts. (*Test*: `testCredentialStoreWaitsForUserToAnswerUnlockPrompt`, commit `44b5088`).
* **KB-114 — Token changed while keyring was locked reverted to stale keyring value upon unlock**: When a user rotated or re-entered a token while the keyring was locked or offline, the new token was saved to `credentials.json` (since `storeInKeyring` failed). In subsequent sessions with an unlocked keyring, `loadToken` queried the keyring first, loaded the old stale token, and ignored the newer file token, leaving the updated token unused and stored in plaintext in the file forever. Fixed by prioritizing `loadFromFile` in `loadToken`, immediately migrating the newer file token to the keyring, overwriting the stale keyring entry, and purging the file token. (*Test*: `testTokenChangedWhileKeyringLockedWinsAfterUnlock`, commit `89996b2`).
* **KB-115 — TUI dashboard activity heatmap truncated recent days on 80-column terminals and hardcoded 28 days**: `TuiApp::renderDashboardView` hardcoded the header `tr("  ── 28-DAY ACTIVITY HEATMAP ──")` despite `getActivityHeatmap` returning 28..34 days (Monday-aligned since KB-99), and concatenated all cells into a single line `2 + 3*days` columns wide (86..104 cols), causing `truncateVisible(..., 80)` to truncate recent days and today. Fixed by formatting the title with the actual day count (`tr("  ── %1-DAY ACTIVITY HEATMAP ──").arg(heatmap.size())`) across all translation dictionaries, and wrapping heatmap rows by week blocks (`(m_cols - 2) / 21 * 7` cells per line) so all cells fit within terminal bounds. (*Test*: `testTuiDashboardHeatmapFitsStandardTerminalAndCountsDaysAccurately`, commit `9b1663f`).
* **KB-116 — PDF billing report stripped project names containing angle brackets**: In `AnalyticsService::exportBillingPdf`, project names were inserted directly into HTML table rows (`<strong>+%1</strong>`) without `toHtmlEscaped()`. If a project tag contained angle brackets (e.g. `+Security<Core>`), `QTextDocument` parsed `<Core>` as an HTML tag, dropping it from the document. Fixed by escaping project names with `toHtmlEscaped()`. (*Test*: `testPdfReportEscapesProjectNames`, commit `9b1663f`).
* **KB-117 — TaskDetailsDialog and TUI displayed comment as posted even when database save failed**: `TaskDetailsDialog.qml` and `TuiApp` previously ignored the return value of `addTaskCommentById` / `addTaskComment`. When database persistence failed (database error, invalid task ID, locked database), `TaskDetailsDialog` still pushed the comment into `taskComments` and wiped the input field, while `TuiApp` reported success. Fixed by verifying the return value in both QML and TUI, aborting UI additions and preserving text on failure, and notifying the user via `tr("Failed to save comment to database.")`. (*Test*: `testPostCommentDoesNotDisplayCommentWhenDbSaveFails`, commit `9b1663f`).
* **KB-118 — CI failed because the KB-116 test needs `pdftotext`, absent from the CI image**: `testPdfReportEscapesProjectNames` ran `QProcess("pdftotext")`, but `.github/workflows/release.yml` installed no `poppler-utils`, so in `debian:trixie-slim` `waitForFinished` returned false and every push to `main` after `9b1663f` was red. Fixed by installing `poppler-utils` in CI; the test now resolves the tool with `QStandardPaths::findExecutable` and fails with an explicit message when it is missing, and checks the exit code. (*Test*: `testPdfReportEscapesProjectNames`, commit `94fa5d9`).
* **KB-119 — KB-116 regression test passed without the KB-116 fix**: the activity description `"Refactor kernel +Security<Core>"` also appears, already escaped, in the detailed task log table, so `pdfText.contains("Security<Core>")` was satisfied even when the project row rendered as `+Security`. Fixed by requiring both occurrences (`count("+Security<Core>") == 2`); verified to fail with `toHtmlEscaped()` removed from the project row. (*Test*: `testPdfReportEscapesProjectNames`, commit `94fa5d9`).
* **KB-120 — KB-117 TUI path was untested**: the TUI half of `testPostCommentDoesNotDisplayCommentWhenDbSaveFails` looked for `"Comment posted"`, a string the TUI never printed, while the comment actually succeeded on the new list's default task. Replaced by a test that makes `UPDATE tasks` fail via a `RAISE(ABORT)` trigger, posts `:comment` on an existing task and expects the failure text instead of the success text; verified to fail with the KB-117 TUI fix reverted. (*Test*: `testTuiCommentReportsDbFailureForExistingTask`, commit `94fa5d9`).
* **KB-121 — Unkeyed GitHub/GitLab tasks stopped opening the link from their description (refactoring regression)**: when `2bd24b8`/`37835db` split `IssueTrackerService::buildIssueBrowserUrl` per provider, `buildGitHubBrowserUrl`/`buildGitLabBrowserUrl` ended with `return {}` and the dispatcher returned that result directly, so the "first http(s) link in the description" fallback was no longer reached (e.g. a local task in a github.com list). Fixed by checking `isEmpty()` like the other providers. (*Test*: `testBrowserUrlFallsBackToDescriptionLinkForUnkeyedGitHubAndGitLabTasks`, commit `a296c89`).
* **KB-122 — `go-for-it-tui` aborted on PDF export**: the TUI binary runs on a `QCoreApplication`, but `AnalyticsService::exportBillingPdf` lays out a `QTextDocument` on `QPdfWriter`, which needs `QFontDatabase` and hence a `QGuiApplication`; dashboard `p` or `:export-pdf` hit `qFatal` (SIGABRT) and left the terminal in raw mode on the alternate screen. Fixed by `AnalyticsService::canExportPdf()`, which makes the export return `false` without a `QGuiApplication`; the TUI shows a translated hint to use `go-for-it-qt --tui`. (*Test*: `testTuiBinarySurvivesPdfExport`, drives the real binary in a PTY with an unreachable session bus, commit `02d5f95`).
* **KB-123 — Docs still described the single `tst_todotxt` binary** after the test split: `CLAUDE.md`, `.github/copilot-instructions.md` and the `FIXES.md` header pointed to `tests/tst_todotxt.cpp` and `friend class ::TestTodoTxt`. Updated to the five `tst_*` binaries, `TestSupport`, and the `src/core/trackers/` layout. (Documentation only, commit `5074e90`).
* **KB-124 — Mini player fell behind or hid upon switching windows on Wayland (KWin / KDE Plasma)**: KB-97 strictly matched `w.pid === applicationPid() && w.caption === windowTitle`. On native Wayland clients, KWin frequently reports `pid = 0` / undefined during initial surface handshake, and appends application or hostname suffixes to `w.caption` (while `w.normalCaption` contains the unadorned title). Consequently, the KWin script never matched, `w.keepAbove = true` was never set on Wayland compositors (which ignore `Qt::WindowStaysOnTopHint`), and the mini player was pushed behind active windows when switching apps even with "Always on Top" checked. Fixed by matching process identity via PID or `resourceClass`/`desktopFileName` (`go-for-it-qt`), matching caption via exact equality, `normalCaption`, substring, and localized mini player keywords, closing the temporary script file before execution, and extending retry timers. (*Test*: `testKWinPinScriptTouchesOnlyOwnMiniPlayerWindow`, commit `8bdb0ee`).
* **KB-125 — TUI :comment <text> or :comment with no task selected misreported "Failed to save comment to database."**: In `TuiCommandHandler.cpp` and `TuiApp.cpp`, posting a comment via `:comment <text>` or `:comment` on an empty list or with no task selected previously set `m_statusMessage` to `tr("Failed to save comment to database.")`, misreporting database persistence failure when no operation had been attempted. Fixed by setting `m_statusMessage = tr("No task selected.")` across all localization dictionaries. (*Test*: `testTuiCommentReportsNoTaskSelectedWhenListIsEmpty`, commit `062206c`).
* **KB-126 — Ukrainian activity heatmap title was grammatically incorrect for 31 ("день") and 32..34 ("дні")**: In `TuiDashboardView.cpp`, the dashboard activity heatmap header called `QCoreApplication::translate("TuiApp", "  ── %1-DAY ACTIVITY HEATMAP ──").arg(heatmap.size())` without passing `n`. With `uk.json` defining `"  ── ТЕПЛОВА КАРТА АКТИВНОСТІ ЗА %1 ДНІВ ──"`, day counts of 31 resulted in "31 ДНІВ" (instead of "31 ДЕНЬ") and 32..34 resulted in "32 ДНІВ" (instead of "32 ДНІ"). Fixed by passing `n` into `QCoreApplication::translate` and introducing Slavic 3-form plural syntax (`one;;few;;many`) into `UkTranslator::translate`, properly declining "день / дні / днів". (*Test*: `testUkTranslatorPluralHandling`, commit `8271358`).
* **KB-127 — Removing a list while keyring was locked left orphaned token in wallet**: When a list was removed (`removeList`) while the Secret Service collection was locked and the unlock prompt was dismissed, `deleteFromKeyring` could not unlock or delete the secret item. As a result, the secret item remained indefinitely in the user's keyring as an orphan. Fixed by introducing `CredentialStore::removeListCredentials`, which records pending deletions for the database scope in `credentials.json` when the keyring is locked or unavailable, and purges the orphaned items from the keyring via `unlockKeyringForCurrentDatabase` upon subsequent unlock. Saving list settings with an empty token while locked continues to preserve the token upon unlock. (*Test*: `testRemovingListWhileKeyringLockedPurgesOrphanAfterUnlock`, commit `4f90c8f`).
* **KB-128 — Jira sync no longer closed the last resolved issue (refactoring regression)**: `95a4e77` inverted the empty-first-page check to `contains("isLast") && !isLast`. Jira Cloud `/search/jql` sends no `total`, so `{"issues":[],"isLast":true}` never set `explicitZeroTotal` and the local task stayed open; an empty page with `isLast:false` was treated as explicitly empty instead. (*Test*: `testJiraEmptyLastPageWithoutTotalReconcilesClosedIssue`, commit `83651f5`).
* **KB-129 — GitLab sync never marked closed issues done (refactoring regression)**: `95a4e77` finalized the issue and MR paginators with an empty item list, and `TrackerPaginator::finalize` sets `limitReached` when `items.size() < total`, so every sync with `X-Total > 0` counted as truncated (the KB-15 test passed only because of that). The KB-15 rule "multi-page stream without `X-Total` is not trusted" was dropped too. Fixed by comparing each stream's `itemsAccumulated` with its first `X-Total` and restoring the no-total rule. (*Tests*: `testGitLabSyncWithTotalHeaderReconcilesClosedIssue`, `testGitLabPaginationShiftWithoutTotalHeaderDoesNotCloseSkippedIssue`, commit `83651f5`).
* **KB-130 — Empty YouTrack result no longer reconciled (refactoring regression)**: `95a4e77` recorded YouTrack pages with total `-1`, and the paginator sets `explicitZeroTotal` only for a total of `0`. An empty, valid first page sets it again. (*Test*: `testYouTrackEmptyResultReconcilesClosedIssue`, commit `83651f5`).
* **KB-131 — KWin pin script also pinned the main window and task dialogs**: the KB-124 fallback matched any own window whose caption contained "Mini"/"Міні" and not "Inbox"/"Today" (e.g. the main window of a list named "Mini sprint", or a task details dialog), set `keepAbove`/`onAllDesktops` on it, and the unpin pass cleared the user's own pin. The script now matches `caption`/`normalCaption` exactly or by prefix of the translated mini player title. (*Test*: new fixtures in `testKWinPinScriptTouchesOnlyOwnMiniPlayerWindow`, commit `afec61a`).
* **KB-132 — Schema migrations ran only for `user_version` 0**: `ad2cf05` returned "up to date" for any stored version between 1 and `CurrentSchemaVersion - 1`, so such a database (and every DB at today's version once the constant is raised) never received missing columns. `createSchema()` now runs a list of `MigrationStep`s in order, stamping the version after each, and leaves databases from newer builds alone. (*Test*: `testDatabaseSchemaMigratesIntermediateUserVersion`, commit `89cedc5`).
* **KB-133 — Failed list removal lost the list's token**: `ListRepository::removeList` removed credentials (and, since `09e3476`, queued a keyring purge) before `DELETE FROM lists`; a failing `DELETE` left the list without its token. Credentials are now removed only once the row is gone. (*Test*: `testFailedListRemovalKeepsToken`, commit `0e613ec`).
* **KB-134 — `fetchRemotePriorities` lost its URL check and fallback (refactoring regression)**: `18a206d` returned YouTrack defaults even without a Server URL and emitted nothing for providers without an implementation. Restored the previous behaviour. (*Test*: `testFetchRemotePrioritiesKeepsUrlCheckAndFallback`, commit `413208a`).
* **KB-135 — Opening a database from a newer build modified it**: `createSchema()` ran `ensureUniqueJiraIndex()` (merging and deleting duplicate `jira_key` rows, creating an index) before reading `user_version`, so the "leave newer databases alone" branch came too late. The repair now runs only for versions up to `CurrentSchemaVersion`. (*Test*: `testDatabaseSchemaLeavesNewerVersionDatabaseUntouched`, commit `39ee9f7`).
* **KB-136 — Failed list save changed the stored token**: `ListRepository::saveList` stored or deleted the token before the `INSERT`/`UPDATE`; on SQL failure a token was kept for a list that was never created, a changed token replaced the working one, or a cleared token was lost. The token is now updated after the row is written. (*Test*: `testFailedListSaveLeavesTokensUnchanged`, commit `0f69726`).
* **KB-137 — Partially failed sync reported as success**: an error on a later page (or in one GitLab stream) only blocked reconciliation; the status still said "Synced N issues from …" and only `listIssuesSynced` was emitted. The received items are still saved, but the status now says the list may be incomplete and `syncFailed` is emitted. (*Test*: `testPartialSyncIsReportedAsFailure`, commit `ca49f0e`).
* **KB-138 — Backup replacement was not atomic**: `exportDatabaseBackup` removed the existing backup before renaming the temporary copy, so a crash in between left no backup. It now uses a single `std::filesystem::rename`. The crash window is not reproducible in a test; `testBackupExportReplacesExistingBackup` covers the replacement behaviour (commit `aa8edd3`).
* **KB-139 — Compiler warning**: unused `vtodoStart` in the Nextcloud status push (`-Wunused-variable`); the build is warning-free with `-Wall -Wextra` (commit `4c27030`). Related test fix: translation coverage tests read keys by iteration because Qt 6.12 `QJsonObject::contains()` misses keys next to escaped ones (commit `44b1f3c`).
* **KB-140 — Mini player pin retry race**: every `configureMiniPlayerWindow` call scheduled KWin pin retries (80/200/500/1000 ms) with the state captured at call time; unpinning shortly after the mini player was shown let the earlier "pin" retries run after the "unpin" script. `DesktopIntegration::schedulePinScript` now stamps a per-window generation and stale retries skip themselves; retries also stop when the window is destroyed. (*Test*: `testKWinPinRetriesOfOlderStateAreCancelled`, verified failing with the generation check removed.)
* **KB-141 — Stale Arch `.SRCINFO`**: since `18e0572` both `PKGBUILD` files fetch `#tag=v${pkgver}`, but both checked-in `.SRCINFO` files still advertised `#branch=main`, so AUR metadata pointed at the moving branch. Both files regenerated with `makepkg --printsrcinfo`. (*Test*: `testArchSourceInfoMatchesPkgbuildReleaseTag`, verified failing with `#branch=main` restored.)
* **KB-142 — Mini player position not remembered**: the mini player reopened wherever the window system put it. Its position is now stored in `SavedState/mini_player_pos` and restored on show if it still lies on a screen. On X11 (and other platforms that report positions) Qt sets and reads it directly; on Wayland, where a client can do neither, a KWin script that stays loaded while the mini player is visible places the window and reports finished interactive moves back over D-Bus (`MiniPlayerPositionReceiver`). KWin 6 hands out `frameGeometry` as a read-only copy, so the script assigns a plain `{x, y, width, height}` object. Checked end-to-end in a nested `kwin_wayland --virtual` on a private bus. (*Tests*: `testMiniPlayerTrackerScriptPlacesAndReportsOnlyOwnWindow` — fails with the copy-and-modify move; `testMiniPlayerWindowRestoresAndSavesItsPosition` — fails with the previous `MiniPlayerWindow.qml`; `testRestorableMiniPlayerPositionNeedsAVisibleScreen`, `testMiniPlayerPositionReportOverDBusIsSaved`, `testKWinTrackerScriptStaysLoadedUntilUnloaded`.)
* **KB-143 — Next/previous task ignored the filter**: `switchToNextTask`/`switchToPreviousTask` (Alt+Down/Up, tray, mini player) stepped through unfiltered source rows and could make a hidden task active. They now move within the filtered list (from a hidden active task to the nearest visible one), and the mini player's buttons use the new `hasNextTask`/`hasPreviousTask`. (*Test*: `testNextAndPreviousTaskFollowTheFilteredList`, fails with the old stepping.)
* **KB-144 — Misleading mini player pin hint**: the 📌 button said "Always on Top Mini-Player (Ctrl+M)", but it only toggles keeping the window on top (Ctrl+M shows/hides the mini player). The hint now says what a click does. (*Test*: `testMiniPlayerPinButtonHintDescribesTheToggle`.)
* **KB-145 — Due reminders fired once per process**: `checkDueReminders` remembered task ids forever, so "due today" was never followed by "overdue", overdue tasks were reminded once however long the app ran, and a postponed due date got no reminder. Each (task, due date, kind) is now reminded once per day. (*Test*: `testDueRemindersRepeatPerDayAndDueDate`, fails at the next-day overdue reminder with the old set.)
* **KB-146 — Stale details/comments after sync**: an empty description (or comment list) in a sync kept the old local value, so text removed on the tracker stayed in the app. Parsers now mark fields their response always carries (`detailsFetched` for all, `commentsFetched` for Jira, YouTrack, Nextcloud) and `upsertRemoteIssueTask` replaces those even when empty; comments of providers that do not send them are kept. (*Test*: `testSyncClearsDetailsAndCommentsRemovedOnTracker`, fails with the old keep-if-empty rule.)
* **KB-147 — Ambient sound not remembered / not retranslated**: the chosen ambient mode and volume were lost on restart, and the timer view's combo kept the old language after a language switch. Both are stored in `go-for-it.ini` (`Ambient/mode`, `Ambient/volume`), unknown modes are ignored, and the combo binds to the new `modes` property re-emitted on language change. (*Tests*: `testAmbientSoundChoiceSurvivesRestart`, `testAmbientComboFollowsLanguageChange`, both fail on the previous code.)
* **KB-148 — Main window position not kept on Wayland**: Qt reports the main window at 0,0 on Wayland, so that was saved and setting x/y is ignored; the window always opened where KWin placed it. The KWin tracker from KB-142 now handles windows by role (`main`, `mini_player`): it places the main window at `SavedState/main_window_pos` and reports finished moves back over D-Bus; on Wayland `persistMainWindowGeometry` saves only the size. (*Tests*: `testMainWindowTrackerScriptPlacesOnlyTheMainWindow`, `testMainWindowGeometrySavesPositionOnlyWherePlatformKnowsIt`, `testWindowPositionReportOverDBusIsSaved`; end to end in a nested `kwin_wayland --virtual`: placed at the saved position, a reported move survives a restart.)
* **KB-149 — KWin scripts silently not run**: KWin 6.8's `loadScript` returns the number of loaded scripts as the id, so after any unload a new script got the id and `/Scripting/Script<id>` path of a script still running, and `run()` re-ran that one: with a window tracker loaded, pin scripts were dropped, and two quick tracker restarts stopped scripts from running at all. Scripts are now started with `Scripting.start()`, which runs every loaded script that has not started; the tracker is also not restarted for the same window and title. Reproduced in a nested KWin; `FakeKWinScripting` now repeats ids like KWin. (*Test*: `testKWinScriptsRunDespiteRepeatedIds`, fails with `run()`; with the realistic fake `testKWinScriptFileStaysReadableUntilRunAndNamesAreUnique` failed too.)
* **KB-150 — Pin button without effect**: on Wayland desktops other than KDE Plasma (GNOME, Sway…) the compositor ignores keep-above requests, so the mini player's 📌 did nothing. It is hidden there (`AppController::windowPinningSupported`). (*Test*: `testWindowPinningSupportedOnlyWhereItWorks`.)

---

## Verification

All listed issues are covered by Qt Test regression tests executed via CTest, except KB-101 (a change to the tests themselves):

```bash
build_dir=$(mktemp -d /tmp/go-for-it-build.XXXXXX)
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build "$build_dir" -j2
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" --output-on-failure
cmake -E remove_directory "$build_dir"
```
