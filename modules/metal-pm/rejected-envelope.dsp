// Actual-Faust NEGATIVE CONTROL, not a musical or shipping entry point.
// Output 1: direct reference. Output 2: old rejected recurrence.
declare name "metal-pm-rejected-envelope-negative-control";
e=library("engine.lib");
process=e.ampReference*e.seen,e.ampRecurrenceRejected*e.seen;
