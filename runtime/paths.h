/* Where the game keeps its files: settings, saves and the game data (the extracted disc). */
#ifndef STW_PATHS_H
#define STW_PATHS_H
#include <stddef.h>
const char *stw_data_dir(void);                       /* STW_HOME, else "." when run from a project folder that already has game/, save/ or settings,
                                                         else ~/Library/Application Support/Spartan Total Warrior Recomp */
void stw_path(char *out, size_t n, const char *rel);  /* <data dir>/<rel> */
const char *stw_game_root(void);                      /* folder holding the extracted disc (default.xbe, *.pak, ...): STW_GAME, the saved choice, or <data dir>/game */
void stw_set_game_root(const char *dir);
/* Checks that <root>/default.xbe is the exact executable this build was made from. Returns 1 if so; otherwise 0 and a reason in err. */
int stw_game_valid(const char *root, char *err, size_t errlen);
int stw_bytes_match_build(const unsigned char *xbe, unsigned len);   /* same check on bytes already in memory (a disc image's default.xbe) */
/* The app keeps its helper files (tools, runtime headers, wheel) in <bundle>/Contents/Resources; a checkout keeps them next to the program in ".". */
const char *stw_resources_dir(void);                  /* STW_RESOURCES, else Contents/Resources when run from an app bundle, else "." */
const char *stw_build_id(void);                       /* identifies this runtime build (Resources/build_id.txt, "dev" in a checkout); part of the prepared-game cache key */
void stw_lifted_path(char *out, size_t n);            /* <data dir>/lifted/<game>-<build>/lifted.dylib (STW_LIFTED overrides) */
const char *stw_supported_xbe_hash(void);             /* the default.xbe hash this build supports (hex) */
#endif
