import terminal
import format

proc arrowSelect*(prompt: string, options: seq[string]): int =
  var selected = 0
  let count = options.len
  if count == 0: return -1

  blankLine()
  step(prompt)
  echo DIM & "  ↑/↓ to move, enter to select" & RESET
  blankLine()

  stdout.write "\e[?25l"

  proc render() =
    stdout.write "\e[" & $count & "A"
    for i, opt in options:
      stdout.write "\e[2K"
      if i == selected:
        echo CYAN & "  ❯ " & BOLD & opt & RESET
      else:
        echo "    " & DIM & opt & RESET

  for i, opt in options:
    if i == selected:
      echo CYAN & "  ❯ " & BOLD & opt & RESET
    else:
      echo "    " & DIM & opt & RESET

  while true:
    let ch = getch()
    case ch
    of '\x1b':
      let ch2 = getch()
      if ch2 == '[':
        let ch3 = getch()
        case ch3
        of 'A':
          if selected > 0: dec selected
        of 'B':
          if selected < count - 1: inc selected
        else: discard
    of '\r', '\n':
      break
    of 'q', '\x03':
      stdout.write "\e[?25h"
      return -1
    of 'k':
      if selected > 0: dec selected
    of 'j':
      if selected < count - 1: inc selected
    else: discard
    render()

  stdout.write "\e[?25h"
  blankLine()
  return selected
