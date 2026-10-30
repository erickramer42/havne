/* File/layer: tests/package/consumer.c | an outside program, strict C
 * Testing: HARNESS — tests/package_check.sh builds it against an
 *          installed core package (ADR-0035 §6): through find_package and
 *          pkg-config from the deb, and from the unpacked tarball. It runs
 *          a mock session and exits 0 once frames arrive.
 *
 * Only the installed headers: if one of them includes a header the
 * package leaves out, or the archive needs a library its CMake package
 * or pkg-config file doesn't name, this fails to build or link. */

#include <ecuio/core.h>
#include <ecuio/version.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_int frames;

static void on_frame(const ecuio_frame_t* frame, void* user_data)
{
    (void)frame;
    (void)user_data;
    atomic_fetch_add(&frames, 1);
}

int main(void)
{
    ecuio_session_t* session = NULL;
    const struct timespec tick = {0, 10 * 1000 * 1000};
    int waited;

    if (strcmp(ecuio_version_string(), ECUIO_VERSION_STRING) != 0) {
        fprintf(stderr, "header %s, library %s\n", ECUIO_VERSION_STRING,
                ecuio_version_string());
        return 1;
    }
    if (ecuio_session_create("mock://package", &session) != ECUIO_OK
        || ecuio_register_frame_callback(session, on_frame, NULL) != ECUIO_OK
        || ecuio_session_start(session) != ECUIO_OK) {
        fprintf(stderr, "session: %s\n", ecuio_last_error_detail());
        return 1;
    }
    for (waited = 0; atomic_load(&frames) < 10 && waited < 500; ++waited)
        nanosleep(&tick, NULL);
    ecuio_session_close(session);
    ecuio_session_destroy(session);
    if (atomic_load(&frames) < 10) {
        fprintf(stderr, "only %d frames in 5 s\n", atomic_load(&frames));
        return 1;
    }
    printf("ecuio %s: %d frames\n", ecuio_version_string(), atomic_load(&frames));
    return 0;
}
