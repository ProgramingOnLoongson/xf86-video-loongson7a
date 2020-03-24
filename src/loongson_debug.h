#ifndef LOONGSON_DEBUG_H_
#define LOONGSON_DEBUG_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xf86.h>
#include <xf86drm.h>
#include <errno.h>

/**
 * This controls whether debug statements (and function "trace" enter/exit)
 * messages are sent to the log file (TRUE) or are ignored (FALSE).
 */
extern _X_EXPORT Bool armsocDebug;


#define TRACE_ENTER()                           \
    do {                                        \
        if (armsocDebug) {                      \
            xf86DrvMsg(-1,                      \
                X_INFO, "%s:%d: Entering\n",    \
                __func__, __LINE__);            \
        }                                       \
    } while (0)


#define TRACE_EXIT()                            \
    do {                                        \
        if (armsocDebug) {                      \
            xf86DrvMsg(-1,                      \
                X_INFO, "%s:%d: Exiting\n",     \
                __func__, __LINE__);            \
        }                                       \
    } while (0)


#define DEBUG_MSG(fmt, ...)                             \
    do {                                                \
        if (armsocDebug) {                              \
            xf86DrvMsg(-1,                              \
                X_INFO, "%s:%d " fmt "\n",              \
                __func__, __LINE__, ##__VA_ARGS__);     \
        }                                               \
    } while (0)




#define INFO_MSG(fmt, ...) \
		do { xf86DrvMsg(pScrn->scrnIndex, X_INFO, fmt "\n",\
				##__VA_ARGS__); } while (0)


#define EARLY_INFO_MSG(fmt, ...) \
		do { xf86Msg(X_INFO, fmt "\n",\
				##__VA_ARGS__); } while (0)


#define CONFIG_MSG(fmt, ...) \
		do { xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, fmt "\n",\
				##__VA_ARGS__); } while (0)


#define WARNING_MSG(fmt, ...) \
		do { xf86DrvMsg(pScrn->scrnIndex, \
				X_WARNING, "WARNING: " fmt "\n",\
				##__VA_ARGS__); \
		} while (0)


#define EARLY_WARNING_MSG(fmt, ...) \
		do { xf86Msg(X_WARNING, "WARNING: " fmt "\n",\
				##__VA_ARGS__); \
		} while (0)


#define ERROR_MSG(fmt, ...) \
		do { xf86DrvMsg(pScrn->scrnIndex, X_ERROR,  \
				"ERROR: " fmt "\n", ##__VA_ARGS__); \
		} while (0)


#define EARLY_ERROR_MSG(fmt, ...) \
		do { xf86Msg(X_ERROR, "ERROR: " fmt "\n",\
				##__VA_ARGS__); \
		} while (0)

void LS_PrepareDebug(ScrnInfoPtr pScrn);

#endif
