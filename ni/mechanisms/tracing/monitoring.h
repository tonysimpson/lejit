/* Tracing through sys.monitoring (PEP 669), which settrace and setprofile
   also use: there is no per-thread flag, and events are instrumented into
   code objects (QUESTIONS 32, D75). */
#error "Ni: running interpreted under sys.monitoring (PEP 669) is not written yet (ni/mechanisms/tracing/monitoring.h; D75)"
