# R12 offline installation and post-install parity

The clean R12 runtime was actually launched from the independent offline test directory
on October 27. The reviewed pre-init gate observed a new cooperative pause acknowledgment
from the existing coordinator/video pair before allowing GPU initialization. Physical
input remained enabled.

The actual native log records verified installation of eight chunks, 14,124,573,292 bytes,
and the private `game/game.iso` with expected XBE SHA1. It loaded 7,081 replacement textures,
played HD logos, advanced its initial GPU fences and rendered the city/traffic. The process
exited normally at 04:33:12 UTC. The same movie worker resumed after the game closed and
advanced Stokmech to 2,096/2,210; it was not restarted to handle that game pause.

The subsequent read-only installed-content audit completed at 22:13:25 UTC:

- All 7,188 installed files match the frozen payload's paths, sizes and SHA256 hashes:
  one ISO, 7,081 PNGs and 106 movies, 14,123,992,085 payload bytes.
- The V2 installed marker binds the exact manifest and installed inventory. No missing,
  extra or linked payload paths were accepted. Runtime files match the pinned ZIP.
- All current protected personal files and authenticated metadata stayed unchanged during
  the audit. Two earlier changes from the later canonical play session are explicitly
  retained; the report does not falsely claim all historical staging bytes still match.
- Private HDD8 routing is derived from authenticated launch arguments, pinned source and
  the actual private directory. The release does not emit the verbose HDD mount log line.

External evidence under the corrected-content release-work directory:
`r12-offline-qa-control-r1/installed-audit-r3/result.json`, SHA256
`75651e052df5e78997895586d976fb63795ee9d47ef356a8bb2b48f645fea5e1`.
The new v3 helper SHA is
`c55c85b8b54a0a14b897a3cad2869062b881ad5c7360d620db5db682c45fd788`;
35 focused path, identity and historical/current baseline checks pass. Earlier helpers
remain preserved: they refused before payload auditing because their readiness conditions
assumed a path/log representation this release does not emit.

This completes installed byte parity, not all native interaction gates. R12 keyboard,
controller and F1 usability still need their own explicit bounded acceptance. Logs showing
menu paths do not establish which input device was used. Publication has not occurred.
The frozen cohort remains 44 strict movies plus 62 historical outputs; this audit does not
change their provenance or repeat the completed broad media decode audit.

The host rebooted at 21:58:23 UTC. The dead encoder's partial movie and temporary frames
were preserved. The partial lacks an MP4 movie index and current tools cannot resume its
encoder state. After fresh game/worker/marker checks, one hidden coordinator (13084) and
video child (7684) restarted the current durable workflow. A new original-derived Stokmech
render advanced; no recovery/adoption of the old partial is claimed. Discover actual live
PIDs before future work. Restart evidence is `coordinator-post-reboot-20261027-r1`.
