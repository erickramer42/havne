# Security Assurance Case for QtTune

## Threat Model

**Adversaries**: Malicious J2534 vendors, compromised adapters, attacker-controlled vehicles, supply-chain compromise of dependencies.

**Assets**: Host system integrity, vehicle ECU safety, user data privacy.

**Attack surfaces**:
1. J2534 vendor DLL loading (untrusted binaries)
2. Adapter firmware (potential malicious devices)
3. Signal decoding pipeline (malformed CAN frames)
4. Surrogate IPC pipe (cross-process communication)
5. Build dependencies (supply chain)

**Out of scope**: ECU writing/flashing (read-only design); network-based attacks (local-only operation).

## Trust Boundaries

| Boundary | What crosses | Defense |
|----------|-------------|---------|
| Adapter → J2534 worker | Raw CAN frames | Frame length ≤64 bytes, protocol validation |
| Worker → Core decode | Decoded frames | Type-checked signal definitions, bounds checks |
| Core → Bridge | Display values | Final display values only, no raw payload exposure |
| Surrogate parent → child | Control commands | Named pipe with kill-on-close job object |
| Build → Runtime | Dependencies | Dependency pinning in CI, reproducible builds |

## Secure Design Principles

| Principle | Application | Evidence |
|-----------|-------------|----------|
| Economy of mechanism | Read-only OBD-II Mode 01; no writes, no DTC clearing | CONTRIBUTING.md Scope |
| Fail-safe defaults | Unknown profile names rejected at session_create | core/src/core.cpp |
| Complete mediation | URI scheme validation before DLL load | CONTRIBUTING.md Input validation |
| Least privilege | Surrogate runs under job object; minimal permissions | SECURITY.md; surrogate client |
| Defense in depth | Tiered testing (Unit/Bridge/HARNESS/Fuzz); ASan/UBSan | docs/testing.md; CMakeLists.txt |
| Append-only ABI | Enum/status codes never renumbered | CONTRIBUTING.md Conventions |

## Common Weaknesses Countered

| Weakness | Countermeasure | Evidence |
|----------|---------------|----------|
| Buffer overflow | Frame buffer capped at 64 bytes; ASan/UBSan in CI | core/include/ecuio/core.h; docs/testing.md |
| Use-after-free | Session closed before destroyed; callbacks snapshot | core/src/core.cpp |
| Integer overflow | Numeric fields validated within OBD-II ranges | CONTRIBUTING.md Input validation |
| Unsafe deserialization | Signal definitions validated before registration | core/src/profile_registry.cpp |
| Insecure dependencies | CI runs with pinned actions; dependency updates via Dependabot | .github/dependabot.yml |
| Injection (protocol) | Unknown URI schemes rejected at parse time | CONTRIBUTING.md Input validation |

**Testing coverage:** Unit tests cover core/ABI logic; Bridge tests verify Qt marshaling; HARNESS tests cover J2534 stack against fake vendor DLL; fuzzers exercise signal decoder under libFuzzer in CI. See docs/testing.md.

**Remaining gaps:** Signed releases (planned for v1.0 per SECURITY.md); formal vulnerability credits (pre-release, no reports received).
