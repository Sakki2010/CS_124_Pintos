#ifndef USERPROG_EXCEPTION_H
#define USERPROG_EXCEPTION_H
#include <inttypes.h>

/*! Page fault error code bits that describe the cause of the exception. @{ */
#define PF_P 0x1    /*!< 0: not-present page. 1: access rights violation. */
#define PF_W 0x2    /*!< 0: read, 1: write. */
#define PF_U 0x4    /*!< 0: kernel, 1: user process. */
/*! @} */

/*! Returned by page_fault in eax to indicate that it's occurred. */
#define PF_ERR ((uint32_t) 0xffffffff)

void exception_init(void);
void exception_print_stats(void);

#endif /* userprog/exception.h */

