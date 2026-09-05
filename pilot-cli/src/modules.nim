## Module install at deploy time (2026-09-05).
##
## `pilot deploy` used to assume ./setup-modules.sh had run: on a fresh machine it started the
## daemon over an empty modules directory and failed minutes later with "storage_module not
## found" / "pilot not responding". Now the modules the agent needs are installed before the
## daemon starts, from the LGX packages the nix build leaves in the store — the same source
## setup-modules.sh uses — and the deploy says exactly which it installed or could not find.
import os, osproc, strutils
import rpc, format

const REQUIRED_MODULES* = ["lez_core", "delivery_module", "storage_module", "pilot"]

proc missingModules*(modulePath: string, required: openArray[string]): seq[string] =
  ## The required modules that have no installed directory under `modulePath`, in the order
  ## given. Pure apart from dirExists, so it is unit-testable on a scratch directory.
  for m in required:
    if not dirExists(modulePath / m): result.add m

proc findInStore(cmd: string): string =
  try:
    result = execProcess("bash", args = ["-c", cmd], options = {poUsePath}).strip()
  except:
    result = ""

proc findLgpm*(): string =
  findInStore("find /nix/store -maxdepth 3 -name lgpm -path '*/bin/*' -type f 2>/dev/null | head -1")

proc findLgx*(name: string): string =
  ## The LGX package a `nix build .#lgx` of that module left in the store.
  findInStore("find /nix/store -maxdepth 2 -name " &
              quoteShell("logos-" & name & "-module-lib*.lgx") & " 2>/dev/null | head -1")

proc installMissingModules*(cfg: Config, missing: seq[string]): bool =
  ## Install each missing module with lgpm from its LGX in the nix store. Reports every
  ## outcome; false when any required module is still missing afterwards (its LGX is not in
  ## the store: build the module stack first).
  let lgpm = findLgpm()
  if lgpm == "":
    fail("lgpm (the module installer) is not in the nix store")
    info("Build the module stack first: cd pilot-module && nix build .#lgx (docs/deployment-guide.md, step 2)")
    return false
  createDir(cfg.modulePath)
  result = true
  for m in missing:
    let lgx = findLgx(m)
    if lgx == "":
      fail("no LGX package for " & m & " in the nix store")
      info("Build it first (docs/deployment-guide.md, step 2) or run ./setup-modules.sh")
      result = false
      continue
    let output = try:
        execProcess("bash", args = ["-c",
          quoteShell(lgpm) & " install --file " & quoteShell(lgx) &
          " --modules-dir " & quoteShell(cfg.modulePath) & " --allow-unsigned 2>&1"],
          options = {poUsePath}).strip()
      except: "lgpm could not be run"
    if dirExists(cfg.modulePath / m):
      ok("installed " & m & " from " & extractFilename(lgx))
    else:
      fail("lgpm did not install " & m & ": " & output[0 ..< min(output.len, 160)])
      result = false
