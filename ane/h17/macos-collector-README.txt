J700 / T8140 macOS PMGR evidence collector

Run on this Neo under macOS, with the SD card mounted:

  /bin/bash "/Volumes/NO NAME/ANE-PMGR-COLLECTOR/macos-collect-pmgr.command"

If the directory has a timestamp suffix, substitute its actual name.
No sudo, developer tools, security-setting changes or network access required.
The script creates a NEW timestamped output directory beside itself. It does
not modify or delete existing files. Bring that directory back on the card.
If Terminal requests access to the removable volume, allow it for this run.

Collected: OS/kernel/model identity, IODeviceTree and IOService property
archives, targeted PMGR/ANE service listings, security status, per-command
errors/status and SHA-256 hashes. The full registry archives may contain
serial numbers: do not publish them unredacted.

Limit: macOS does not normally expose kernel-object memory to a local script.
This captures registry evidence, not the private runtime RegMap/RegGroup
tables. If those are absent, further collection requires a separately planned
kernel-debugging session, matching symbols and verified object layouts.
An ordinary local `lldb -k` session is not a turnkey solution on stock macOS.
Do not change SIP or boot settings for this collector.
