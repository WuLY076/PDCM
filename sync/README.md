# GitLab export boundary

The authoritative production-code boundary is the top-level `product/`
directory. Future one-way synchronization should export that directory by
allowlist, for example by splitting its Git history or by creating a clean
archive from it.

`testkit/` is GitHub-only. Do not copy the full repository and then delete
test files; that approach is intentionally unsupported because it can leak
MockProvider code or test catalogs.

Before exporting, run `tools/sync/verify-sync-boundary.sh`.
