#pragma once
typedef int critical_section_t;
static inline void critical_section_init(critical_section_t *c) { (void)c; }
static inline void critical_section_enter_blocking(critical_section_t *c) { (void)c; }
static inline void critical_section_exit(critical_section_t *c) { (void)c; }
