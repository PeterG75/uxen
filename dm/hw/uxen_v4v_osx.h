#ifndef _UXEN_V4V_OSX_H_
#define _UXEN_V4V_OSX_H_

#include <stdint.h>
#include <sys/types.h>
#include <string.h>
#include <xen/v4v.h>
#include "../../osx/uxenv4vservice/uxenv4vlib.h"
#include "../ioh.h"

typedef struct v4v_context {
    union {
        v4v_channel_t;
        v4v_channel_t v4v_channel;
    };
    ioh_event recv_event;
} v4v_context_t;

#endif
