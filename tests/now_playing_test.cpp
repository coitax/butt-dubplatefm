/*
 * Spec for the Linux now-playing helpers.
 *
 * getCurrentTrackFunctionFromId selects the same functions the FLTK app
 * timer calls. Each MPRIS helper must:
 *   - return "artist - title" when both xesam:artist and xesam:title are
 *     present (several artists joined with ", ");
 *   - return NULL when playback is Stopped;
 *   - return NULL, without crashing, when Playing/Paused metadata omits
 *     xesam:artist or xesam:title.
 * The returned text is the whole allocation: no bytes before the artist.
 *
 * argv: now_playing_test <service-id> <expected>
 * expected is the literal string, or NULL when the helper must return NULL.
 * Service ids match the Linux player menu: 0 Rhythmbox, 2 Clementine,
 * 3 Spotify, 4 Cantata. Id 1 is Banshee and is not covered here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

typedef const char *(*currentTrackFunction)(void);
currentTrackFunction getCurrentTrackFunctionFromId(int i);

int main(int argc, char **argv) {
    int id;
    const char *expected;
    currentTrackFunction fn;
    const char *track;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <service-id> <expected-or-NULL>\n", argv[0]);
        return 2;
    }

    /*
     * Fresh malloc blocks are otherwise zero-filled by the kernel. Fill
     * them with a non-zero byte so a buffer that is not terminated before
     * strcat cannot look like a valid empty string.
     */
    if (mallopt(M_PERTURB, 85) != 1) {
        fprintf(stderr, "mallopt M_PERTURB failed\n");
        return 2;
    }

    id = atoi(argv[1]);
    expected = argv[2];
    fn = getCurrentTrackFunctionFromId(id);
    if (fn == NULL) {
        fprintf(stderr, "service id %d has no handler\n", id);
        return 2;
    }

    alarm(8);
    fprintf(stderr, "calling service %d\n", id);
    fflush(stderr);
    track = fn();

    if (strcmp(expected, "NULL") == 0) {
        if (track != NULL) {
            fprintf(stderr, "expected NULL, got [%s]\n", track);
            return 1;
        }
        printf("ok NULL\n");
        return 0;
    }

    if (track == NULL || strcmp(track, expected) != 0) {
        fprintf(stderr, "expected [%s], got [%s]\n",
                expected, track != NULL ? track : "(null)");
        return 1;
    }
    printf("ok [%s]\n", track);
    return 0;
}
