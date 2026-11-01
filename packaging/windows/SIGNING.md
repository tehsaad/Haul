# Signing the Windows build

Haul signs through the [SignPath Foundation](https://signpath.org/) programme. Until the settings below exist, the workflow builds exactly as before and skips signing.

## After approval

1. In SignPath, create project `haul` with signing policy `release-signing`, which requires manual approval.
2. Add two artifact configurations by pasting the files in `packaging/windows/signpath/`: `executables` and `installer`.
3. In GitHub under Settings > Secrets and variables > Actions, add:
   - secret `SIGNPATH_API_TOKEN`
   - variable `SIGNPATH_ORGANIZATION_ID`
4. Run the Windows workflow. It stops twice for approval in SignPath: once for the programs and once for the installer.

## What gets signed

`haul.exe`, `haul-native-host.exe` and the installer. Library DLLs aren't signed, because they aren't built from this project.

`Uninstall.exe` is written by the installer at install time, so it stays unsigned for now. Signing it needs an extra build pass, which can be added once signing works.

## Checking a signature

```powershell
Get-AuthenticodeSignature .\Haul-<version>-Setup.exe
```

`Status` should be `Valid`, with SignPath Foundation as the publisher.
