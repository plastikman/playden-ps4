# Local process probe

This throwaway probe records what a title and a child process can do when the
title starts the child with `sceSystemServiceAddLocalProcess`. It is not part
of Playden's application.

For this PS4 checkout, `./probes/local-process/build.sh` builds a native parent
and signed child with the bundled OpenOrbis SDK. The parent and child log to
`/app0/parent.log` and `/app0/child.log`. Set `PROBE_MAC_IP` to also send UDP
copies to a host on the local network. Add `--pkg` to make an installable probe
package; the default is to build only.

The original Prospero research scripts are retained as
`build-ps5.sh` and `build-ps5-shared.sh`. The shared-Wine-server probe is still
PS5-specific and has not been adapted for PS4; this PS4 build does not depend on
Orbis-Win.
