#ifndef _UTIL_H_
#define _UTIL_H_

#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

int openpty(int *, int *, char *, struct termios *, struct winsize *);
int forkpty(int *, char *, struct termios *, struct winsize *);
const char *getprogname(void);

#endif
