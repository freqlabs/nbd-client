#ifndef _CHECK_H_
#define _CHECK_H_

#include <libcasper.h>
#include <casper/cap_syslog.h>

enum { SUCCESS = 0, MOREDATA = 1, FAILURE = -1, TIMEOUT = -1 };

extern cap_channel_t *system_syslog$;
#define log(...) cap_syslog(system_syslog$, __VA_ARGS__)

#endif /* #ifndef _CHECK_H_ */
