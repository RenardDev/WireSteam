# Publishing releases

Build the intended source revision with `.\build.ps1 -Installer`. Keep SDK files, private profiles, keys, build caches and `dist/` out of source commits.

WireSteam source uses the [MIT license](LICENSE). Include it in source and binary distributions. Review [third-party terms](THIRD_PARTY_NOTICES.md) separately; a project license does not replace dependency licenses.

## Release checklist

1. Update the desktop project's three-part version; preserve the MSI UpgradeCode and component identities.
2. Run the affected checks described in [Contributing](CONTRIBUTING.md), review the source diff and run `git diff --check`.
3. Test installation, upgrade, rollback and removal in clean Windows VMs. Check VPN connectivity between two computers separately.
4. Review required dependency notices. Sign the MSI if a publisher certificate is available and regenerate its checksum after signing.
5. Attach `WireSteam-<version>-x64.msi` and its `.sha256` file, `WireSteam-win64.zip`, and `SHA256SUMS.txt` to the matching GitHub Release. Include `WireSteam-speedtest-win64.zip` if distributing the standalone tools.

Release notes should state supported systems, relevant changes and known limitations. Automated package inspection does not test a live installation or Internet gateway.

Create source archives from a reviewed Git commit. Never upload a ZIP of the working directory, private configurations, runtime files, SDK archives, PDBs or crash dumps.
