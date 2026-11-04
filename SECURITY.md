# Security

If you find a security problem in the JetKVM Mini, tell us privately first. We fix the problem, then publish the details with you. This file covers the Mini's firmware; [jetkvm.com/security](https://jetkvm.com/security) is the policy for every JetKVM product.

## Report a vulnerability

- **On GitHub (preferred):** on this repository's **Security** tab, select **Report a vulnerability**.
- **By email:** write to security@jetkvm.com, and encrypt sensitive details with our OpenPGP key `061C 13D3 B358 5443 FEBB 6755 4C75 3D10 CD08 5463` (`gpg --locate-keys security@jetkvm.com`).

Do not open a public issue for it. Include the firmware version, the board (Ethernet or Wi-Fi), the steps to reproduce the problem, the impact, and a proof of concept if you have one.

## Scope

The Mini controls the computer it is plugged into, so a report is in scope when it gets past the Mini's access control (its local password, or JetKVM Cloud for an adopted Mini) to any of these:

- the computer's screen and sound
- its keyboard and mouse
- virtual media, which can boot the computer from a disk image
- OS Services, which can open a shell as the signed-in user, read and set the clipboard, and write files to the Downloads folder
- firmware updates: a stable release must install only when its GPG signature verifies

Out of scope:

- physical access to the Mini: its USB and debug ports can flash any firmware, and its TF card can be read
- a Mini set up without a password, reached by someone on the same network
- pre-release and custom firmware versions, which install without a signature check
- denial-of-service attacks and load testing, and scanner reports that show no real impact

## After you report

We confirm that we received your report, investigate it, and keep you informed. We agree a disclosure date with you; please give us a reasonable time to release a fix before you publish. We do not run a bug bounty. With your permission, we credit you in the security advisory and the release notes.

If you act in good faith and follow this policy, we consider your research authorized; the [safe harbor](https://jetkvm.com/security) terms on jetkvm.com apply. Test only on devices and accounts that you own or have permission to test.
