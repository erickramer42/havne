// File/layer: core/src/j2534_surrogate_pipe.cpp | C++17, Qt-free, Win32-guarded:
//             the surrogate child's pipe, as ecuio_j2534_sur_open makes it
// Testing: HARNESS: J2534PipeTest (test_j2534_routing, tests/test_j2534_pipe.cpp)

#include "ecuio/j2534_surrogate.h"

#ifdef _WIN32

#include <windows.h>

#include "ecuio/surrogate_protocol.h"
#include "win_utf8.h"

namespace {

/* The pipe's security: full access for this process's user only (the
 * default DACL lets Everyone connect before the child and take the only
 * instance). The child runs under the same token. Lives on the caller's
 * stack until CreateNamedPipe returns. */
struct user_only_security {
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    alignas(DWORD) BYTE
        acl[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE];
    alignas(void*) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
};

bool init_user_only(user_only_security* s)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD len = 0;
    const BOOL got =
        GetTokenInformation(token, TokenUser, s->user, sizeof(s->user), &len);
    CloseHandle(token);
    if (!got) {
        return false;
    }
    PSID sid = reinterpret_cast<TOKEN_USER*>(s->user)->User.Sid;
    ACL* acl = reinterpret_cast<ACL*>(s->acl);
    if (!InitializeAcl(acl, sizeof(s->acl), ACL_REVISION) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, sid) ||
        !InitializeSecurityDescriptor(&s->sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&s->sd, TRUE, acl, FALSE)) {
        return false;
    }
    s->sa.nLength = sizeof(s->sa);
    s->sa.lpSecurityDescriptor = &s->sd;
    s->sa.bInheritHandle = FALSE;
    return true;
}

} // namespace

extern "C" ecuio_status_t ecuio_j2534_sur_create_pipe(const char* name,
                                                      void** out_pipe,
                                                      uint32_t* out_err)
{
    if (name == nullptr || out_pipe == nullptr || out_err == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out_pipe = nullptr;
    *out_err = 0;
    wchar_t wname[128];
    user_only_security sec = {};
    if (!ecuio_utf8_to_wide(name, wname, _countof(wname))) {
        *out_err = ERROR_INVALID_NAME;
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    if (!init_user_only(&sec)) {
        *out_err = GetLastError();
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    /* Local, user-only, first instance only: no other process can sit on
     * the child's end. Its out-buffer holds a whole reply. */
    HANDLE pipe = CreateNamedPipeW(wname,
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                       FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                       PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, sizeof(ecuio_sur_request_t),
                                   sizeof(ecuio_sur_response_t) +
                                       ECUIO_SUR_MAX_FRAMES_PER_READ *
                                           sizeof(ecuio_pt_rx_t),
                                   0, &sec.sa);
    if (pipe == INVALID_HANDLE_VALUE) {
        *out_err = GetLastError();
        return ECUIO_ERR_SURROGATE_FAILED;
    }
    *out_pipe = pipe;
    return ECUIO_OK;
}

#else

extern "C" ecuio_status_t ecuio_j2534_sur_create_pipe(const char*, void**,
                                                      uint32_t*)
{
    return ECUIO_ERR_NOT_IMPLEMENTED;
}

#endif
