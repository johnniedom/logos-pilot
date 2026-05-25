import strutils

const
  GREEN* = "\e[0;32m"
  RED* = "\e[0;31m"
  YELLOW* = "\e[0;33m"
  BLUE* = "\e[0;34m"
  CYAN* = "\e[0;36m"
  MAGENTA* = "\e[0;35m"
  BOLD* = "\e[1m"
  DIM* = "\e[2m"
  RESET* = "\e[0m"

proc ok*(msg: string) =
  echo GREEN & "✓ " & RESET & msg

proc fail*(msg: string) =
  stderr.writeLine RED & "✗ " & RESET & msg

proc warn*(msg: string) =
  echo YELLOW & "! " & RESET & msg

proc info*(msg: string) =
  echo DIM & "→ " & RESET & msg

proc step*(msg: string) =
  echo BOLD & msg & RESET

proc kv*(key, val: string, indent = 2, width = 14) =
  echo spaces(indent) & DIM & alignLeft(key, width) & RESET & val

proc kvColor*(key, val, color: string, indent = 2, width = 14) =
  echo spaces(indent) & DIM & alignLeft(key, width) & RESET & color & val & RESET

proc badge*(label, color: string): string =
  color & "[" & label & "]" & RESET

proc truncStr*(s: string, maxLen = 16): string =
  if s.len > maxLen: s[0 ..< maxLen - 3] & "..."
  else: s

proc hrule*(width = 50) =
  echo DIM & repeat("─", width) & RESET

proc header*(title: string) =
  echo ""
  echo BOLD & title & RESET
  hrule()

proc blankLine*() =
  echo ""

proc tableRow*(cols: varargs[tuple[text: string, width: int]]) =
  var line = "  "
  for col in cols:
    line &= alignLeft(col.text, col.width)
  echo line

proc tableHeader*(cols: varargs[tuple[text: string, width: int]]) =
  var line = "  "
  for col in cols:
    line &= BOLD & alignLeft(col.text, col.width) & RESET
  echo line
  var rule = "  "
  for col in cols:
    rule &= repeat("─", col.width)
  echo DIM & rule & RESET

const SPIN_FRAMES* = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

proc spinner*(msg: string) =
  stdout.write DIM & "⠋ " & RESET & msg & "\r"
  stdout.flushFile()

proc spinTick*(msg: string, frame: int) =
  let ch = SPIN_FRAMES[frame mod SPIN_FRAMES.len]
  stdout.write "\e[2K" & DIM & ch & " " & RESET & msg & "\r"
  stdout.flushFile()

proc clearLine*() =
  stdout.write "\e[2K\r"
  stdout.flushFile()
