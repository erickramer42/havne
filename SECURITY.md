# Security Policy

## Supported Versions

QtTune is in active pre-release development (v0.1.0–v0.3.0). There are no supported production versions at this time.

| Version | Supported          |
| ------- | ------------------ |
| v0.3.x  | :white_check_mark: (development) |
| < v0.3  | :x:                |

Once v1.0.0 is released, this table will document long-term support windows for each release line.

## Reporting a Vulnerability

If you discover a potential security vulnerability in QtTune (e.g., code scanning findings, supply-chain risks, or unexpected behavior that could compromise host systems), please report it using **GitHub Security Advisories**:

1. Go to https://github.com/erickramer42/qttune/security/advisories/new
2. Select **Report a vulnerability**
3. Describe the issue in detail with reproduction steps

### What to Expect

- **Initial acknowledgment:** Within 7 days
- **Status update:** Within 30 days on remediation progress
- **Public disclosure:** After mitigation is available, unless ongoing investigation requires delay

### Vulnerability Categories

QtTune operates in a unique threat model:

| Category | Response Path |
|----------|---------------|
| Code security (buffer overflows, memory safety) | Fixed via CVE assignment if severe |
| Supply-chain (dependency, action pinning) | Addressed in next release |
| **Vehicle safety (ECU flashing, protocol bypass)** | See disclaimer below |
| Privacy (logging, credential exposure) | Patched and disclosed |

### Important Disclaimer

This project interacts with automotive diagnostic protocols that can affect vehicle safety systems. Many "vulnerabilities" discovered in QtTune are **intentional design decisions** related to ECU communication—not security flaws to be fixed.

If your report concerns:
- Security gateway bypass mechanisms
- ECU authentication/workaround discovery
- Emissions compliance circumvention

Please note: **QtTune is for research and understanding only**. The maintainers will evaluate whether the finding represents a genuine vulnerability versus expected protocol analysis behavior. Reports about third-party tool vulnerabilities (vendor drivers, J2534 interfaces) should be directed to those manufacturers, not QtTune.

## Security Features in Progress

As development proceeds, the following will be implemented:

- [ ] Secure storage for any credentials (currently: none stored)
- [ ] Integrity verification of calibration files
- [ ] Signed release artifacts (when v1.0 ships)

Until then, the development workflow (branch protection, least-privilege permissions, CodeQL scanning) provides baseline security assurances.

---

*This document complements [DISCLAIMER.md](DISCLAIMER.md), which covers legal, regulatory, and safety caveats.*
