#ifndef RR_ENHANCEMENTS_H
#define RR_ENHANCEMENTS_H
#include <stdint.h>
#include "game/rr_game.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Runtime-resolved managed fields/methods, no downloaded offset profile. */
int rr_features_start(void);
void rr_features_feed(int live, const RR_CAMERA* camera);
void rr_features_silent(int on, RR_V3 point);
void rr_features_target_state(RR_PLAYER* player);
/* 0 recoil, 1 spread, 2 flash, 3 reveal, 4 silent */
const char* rr_features_status(int feature);
const char* rr_features_runtime_status(void);
#ifdef __cplusplus
}
#endif
#endif
