#ifndef PLAYER_PEEK_H
#define PLAYER_PEEK_H

#include "scripthook.h"

enum ShPlayerPeekResult {
    PEEK_OK,
    PEEK_LOCK_BUSY,
    PEEK_LOOKUP_MISS
};

enum ShPlayerPeekResult ShPeekPlayerReason(ShPlayer *out);

#endif
