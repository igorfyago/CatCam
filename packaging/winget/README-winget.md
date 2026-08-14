# winget submission, when the signed release exists

Preconditions, in order:
1. v1.2.0 release published on GitHub with CatCamSetup.exe attached.
2. The exe is code-signed (unsigned installers attract moderation friction
   and defeat the point: SmartScreen reputation).

Steps:
1. Fill `InstallerSha256` in `1.2.0/IgorYago.CatCam.installer.yaml` from the
   exe that is actually on the release:
   `certutil -hashfile CatCamSetup.exe SHA256`
2. Validate locally: `winget validate 1.2.0` and
   `winget install --manifest 1.2.0` on a throwaway VM.
3. Fork microsoft/winget-pkgs, copy the three yaml files to
   `manifests/i/IgorYago/CatCam/1.2.0/`, open the PR. The pipeline bot does
   the rest; expect a day or two.

New versions later: copy the folder, bump versions, refresh the hash and URL
(wingetcreate update IgorYago.CatCam does this in one command).
