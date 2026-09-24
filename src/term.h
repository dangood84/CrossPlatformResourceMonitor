#ifndef TERM_H
#define TERM_H

/* Raw-ish terminal: alt screen, hidden cursor, non-blocking key reads.
 * --once and make test never call these; they print to a normal stdout. */

int  term_init(void);
void term_restore(void);
int  term_poll_key(int timeout_ms);   /* 0 if none; 'q', ' ', 3 (Ctrl+C) */
void term_size(int *cols, int *rows); /* defaults 80x24 if ioctl fails */
void term_begin_frame(void);
int  term_is_tty(void);

#endif /* TERM_H */
