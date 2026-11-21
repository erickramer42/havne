// File/layer: tests/test_j2534_pipe.cpp | GoogleTest, Windows only
// Testing: HARNESS: J2534PipeTest (test_j2534_routing): the surrogate
//          pipe's protections, on the pipe ecuio_j2534_sur_create_pipe
//          makes for every session

#include <gtest/gtest.h>

#include "ecuio/j2534_surrogate.h"

#include <windows.h>
#include <aclapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

/* A pipe name no other case or run takes */
std::string pipe_name(const char* what)
{
    static unsigned n = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "\\\\.\\pipe\\ecuio-pipetest-%lu-%u-%s",
             GetCurrentProcessId(), ++n, what);
    return buf;
}

/* The names are ASCII */
std::wstring wide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

/* The same pipe through the SMB redirector, as a client on another
 * machine reaches it */
std::wstring remote(const std::string& name)
{
    return L"\\\\localhost\\pipe\\" + wide(name.substr(9));
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle()
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
            CloseHandle(h);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

/* The session's pipe: INVALID_HANDLE_VALUE, with *err, when refused */
HANDLE made(const std::string& name, uint32_t* err)
{
    void* p = nullptr;
    uint32_t e = 0;
    const ecuio_status_t st = ecuio_j2534_sur_create_pipe(name.c_str(), &p, &e);
    *err = e;
    if (st != ECUIO_OK) {
        EXPECT_EQ(p, nullptr);
        EXPECT_EQ(st, ECUIO_ERR_SURROGATE_FAILED);
        return INVALID_HANDLE_VALUE;
    }
    return static_cast<HANDLE>(p);
}

/* A pipe of the test's own, open to remote clients */
HANDLE plain_pipe(const std::string& name, DWORD instances)
{
    return CreateNamedPipeW(wide(name).c_str(),
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                PIPE_ACCEPT_REMOTE_CLIENTS,
                            instances, 4096, 4096, 0, nullptr);
}

HANDLE client(const std::wstring& path)
{
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

std::vector<BYTE> user_sid()
{
    HANDLE token = nullptr;
    std::vector<BYTE> buf(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE);
    DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    const BOOL got = GetTokenInformation(token, TokenUser, buf.data(),
                                         static_cast<DWORD>(buf.size()), &len);
    CloseHandle(token);
    if (!got) {
        return {};
    }
    const PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    const BYTE* at = static_cast<const BYTE*>(sid);
    return std::vector<BYTE>(at, at + GetLengthSid(sid));
}

} // namespace

/* The default DACL lets Everyone open the pipe, so a process of another
 * user could take its one instance before the child: the pipe's DACL
 * holds one entry, full access for the user whose session it is */
TEST(J2534PipeTest, OnlyThisUserMayOpenIt)
{
    uint32_t err = 0;
    Handle pipe(made(pipe_name("dacl"), &err));
    ASSERT_TRUE(pipe.ok()) << "Windows error " << err;

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    ASSERT_EQ(GetSecurityInfo(pipe.h, SE_KERNEL_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &dacl, nullptr, &sd),
              static_cast<DWORD>(ERROR_SUCCESS));
    ASSERT_NE(dacl, nullptr) << "a NULL DACL lets everyone in";
    ACL_SIZE_INFORMATION size = {};
    ASSERT_TRUE(
        GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation));
    EXPECT_EQ(size.AceCount, 1u);
    void* ace = nullptr;
    ASSERT_TRUE(GetAce(dacl, 0, &ace));
    const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
    EXPECT_EQ(allowed->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    EXPECT_EQ(allowed->Mask & FILE_ALL_ACCESS,
              static_cast<DWORD>(FILE_ALL_ACCESS));
    std::vector<BYTE> me = user_sid();
    ASSERT_FALSE(me.empty());
    EXPECT_TRUE(EqualSid(const_cast<DWORD*>(&allowed->SidStart), me.data()));
    LocalFree(sd);
}

/* One instance: once the child holds it, neither a second instance of
 * the name nor a second client gets in */
TEST(J2534PipeTest, ItHasOneInstanceForOneClient)
{
    const std::string name = pipe_name("one");
    uint32_t err = 0;
    Handle pipe(made(name, &err));
    ASSERT_TRUE(pipe.ok()) << "Windows error " << err;
    DWORD instances = 0;
    ASSERT_TRUE(
        GetNamedPipeInfo(pipe.h, nullptr, nullptr, nullptr, &instances));
    EXPECT_EQ(instances, 1u);

    Handle again(plain_pipe(name, 1));
    EXPECT_FALSE(again.ok()) << "a second instance of the name was made";

    Handle first(client(wide(name)));
    ASSERT_TRUE(first.ok()) << "Windows error " << GetLastError();
    Handle second(client(wide(name)));
    EXPECT_FALSE(second.ok()) << "a second client got in";
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_PIPE_BUSY));
}

/* A process that made the name first, open to more instances, would
 * read every request the client writes: the session's create is
 * refused rather than joining it (FILE_FLAG_FIRST_PIPE_INSTANCE) */
TEST(J2534PipeTest, ANameTakenFirstIsRefused)
{
    const std::string name = pipe_name("squat");
    Handle squatter(plain_pipe(name, PIPE_UNLIMITED_INSTANCES));
    ASSERT_TRUE(squatter.ok()) << "Windows error " << GetLastError();

    uint32_t err = 0;
    Handle pipe(made(name, &err));
    EXPECT_FALSE(pipe.ok()) << "the session joined a pipe it didn't make";
    EXPECT_EQ(err, static_cast<uint32_t>(ERROR_ACCESS_DENIED));
}

/* A client through the network (here the SMB loopback, \\localhost) is
 * refused (PIPE_REJECT_REMOTE_CLIENTS), while a local one gets in; a
 * pipe of the test's own, open to remote clients, shows the loopback
 * reaches pipes on this machine, or the case skips with its reason */
TEST(J2534PipeTest, ARemoteClientIsRefused)
{
    const std::string open_name = pipe_name("remote-open");
    Handle open_pipe(plain_pipe(open_name, 1));
    ASSERT_TRUE(open_pipe.ok()) << "Windows error " << GetLastError();
    Handle reached(client(remote(open_name)));
    if (!reached.ok()) {
        GTEST_SKIP() << "the SMB loopback reaches no pipe here (Windows error "
                     << GetLastError() << "), so no remote client can be made";
    }

    const std::string name = pipe_name("remote");
    uint32_t err = 0;
    Handle pipe(made(name, &err));
    ASSERT_TRUE(pipe.ok()) << "Windows error " << err;
    Handle from_afar(client(remote(name)));
    EXPECT_FALSE(from_afar.ok()) << "a remote client got in";
    Handle local(client(wide(name)));
    EXPECT_TRUE(local.ok()) << "Windows error " << GetLastError();
}

/* Arguments and a name no pipe can have */
TEST(J2534PipeTest, ABadNameIsAFailedCreate)
{
    void* p = nullptr;
    uint32_t err = 0;
    EXPECT_EQ(ecuio_j2534_sur_create_pipe(nullptr, &p, &err),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_create_pipe("\\\\.\\pipe\\x", nullptr, &err),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_j2534_sur_create_pipe("\\\\.\\pipe\\x", &p, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(made("not a pipe name", &err), INVALID_HANDLE_VALUE);
    EXPECT_NE(err, 0u);
}
