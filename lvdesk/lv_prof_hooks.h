/*
 * Aggregating profiler hooks for LVGL.
 *
 * LVGL instruments its internals with LV_PROFILER_BEGIN/END pairs and lets the
 * integrator supply the implementation through LV_PROFILER_INCLUDE. The
 * built-in one writes a trace buffer meant for external tooling; what is
 * needed here is far simpler - a total and a count per section, printed
 * periodically - because the question is only "which part of lv_timer_handler
 * costs 2.2 ms during pointer motion".
 *
 * This exists because there is no perf, no ftrace and no PMU on this board, so
 * an aggregate has to come from inside the process. It costs two
 * clock_gettime() calls per instrumented section and is inert unless
 * LVDESK_PROF is set - but it is still a diagnostic, not something to leave on
 * while measuring anything else.
 */
#ifndef LV_PROF_HOOKS_H
#define LV_PROF_HOOKS_H

void lvp_begin(const char *tag);
void lvp_end(const char *tag);

#define LV_PROFILER_BEGIN            lvp_begin(__func__)
#define LV_PROFILER_END              lvp_end(__func__)
#define LV_PROFILER_BEGIN_TAG(tag)   lvp_begin(tag)
#define LV_PROFILER_END_TAG(tag)     lvp_end(tag)

#endif /* LV_PROF_HOOKS_H */
