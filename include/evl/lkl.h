#ifndef _EVL_LKL_H
#define _EVL_LKL_H

#include <linux/types.h>
#include <linux/time64.h>

struct rb_node;
struct rb_root;

int lkl_evl_strcmp(const char *a, const char *b);
void lkl_evl_rb_insert_color(struct rb_node *node, struct rb_root *root);
unsigned long lkl_evl_find_first_bit(const unsigned long *addr,
                                     unsigned long size);
struct timespec64 lkl_evl_ns_to_timespec64(s64 nsec);
int lkl_evl_strncmp(const char *a, const char *b, size_t count);
char *lkl_evl_strncpy(char *dest, const char *src, size_t count);
char *lkl_evl_strsep(char **s, const char *ct);
long lkl_evl_simple_strtol(const char *cp, char **endp, unsigned int base);
u64 lkl_evl_hashlen_string(const void *salt, const char *name);
void lkl_evl_rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *lkl_evl_rb_next(const struct rb_node *node);
void lkl_evl_rb_replace_node(struct rb_node *victim, struct rb_node *new,
                             struct rb_root *root);
unsigned long lkl_evl_find_first_zero_bit(const unsigned long *addr,
                                          unsigned long size);
unsigned long lkl_evl_int_sqrt(unsigned long x);

#define evl_ktime_to_timespec64(kt)	lkl_evl_ns_to_timespec64(kt)

/* Host services lkl.o depends on, implemented in kernel/evl/lkl_host.c. */
int lkl_printf(const char *fmt, ...);
void lkl_bug(const char *fmt, ...);

#endif /* !_EVL_LKL_H */