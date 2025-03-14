# debug_arcto.gdb - GDB script for debugging QPainterPath::arcTo NaN warnings
#
# USAGE OPTIONS:
#
# 1. Attach to already running Bitcoin-Qt process:
#    gdb -p $(pidof bitcoin-qt)
#    (gdb) source debug_arcto.gdb
#
# 2. If binary was not compiled with debugging symbols (-g flag),
#    some functionality may be limited. The script will still attempt
#    to break on function names, but variable inspection may not work fully.
#
# 3. After loading the script, allow the application to continue:
#    (gdb) continue
#
# 4. The script will automatically break on Qt warning functions and QPainterPath::arcTo
#    functions when NaN values are detected and print relevant debugging information.
#
# 5. To continue after a breakpoint hit:
#    (gdb) continue
#
# 6. To disable all breakpoints:
#    (gdb) disable
#
# 7. To view full Qt warning messages when they occur:
#    (gdb) set print elements 0

# Attempt to detect if debugging symbols are available and inform the user
echo Checking for debugging information...\n
set $have_debug_symbols = 0
python
try:
  gdb.execute("info functions QPainterPath::arcTo")
  gdb.execute("set $have_debug_symbols = 1")
except:
  gdb.execute("echo Limited debugging information available. Some breakpoints may not work.\n")
  gdb.execute("echo Try using a binary compiled with -g flag for better results.\n")
end

# Helper function to check if debugging symbols are available
define check_debug_symbols
  if $have_debug_symbols == 0
    echo Warning: Limited debugging information. Some features may not work.\n
    echo This may be because the binary was not compiled with -g flag.\n
  end
end

# Set a breakpoint on Qt warning message handling functions
catch throw
commands
  # Check if this is a Qt warning/error message
  if $_streq(strstr($_exception, "QPainterPath::arcTo"), "QPainterPath::arcTo")
    echo \n--- QPainterPath::arcTo exception/warning detected! ---\n
    printf "Message: %s\n", $_exception
    bt full
  else
    continue
  end
end

# Try to break on Qt message functions - these might be available even without full debug symbols
break qWarning
commands
  echo \n--- Qt Warning Function Called ---\n
  bt
  # Check if this is related to arcTo
  if $_streq(strstr($arg0, "arcTo"), "arcTo")
    echo \n!!! arcTo warning detected !!!\n
    printf "Warning message: %s\n", $arg0
    bt full
  else
    continue
  end
end

# Set breakpoints on QPainterPath related functions - might require debug symbols
break QPainterPath::arcTo
commands
  check_debug_symbols
  echo \n--- QPainterPath::arcTo called ---\n
  # Print backtrace
  bt full
  # Print function arguments (if debug symbols available)
  info args
  # Print local variables (if debug symbols available)
  info locals
  echo \n
end

# Set breakpoints on other QPainterPath functions that might call arcTo internally
break QPainterPath::cubicTo
commands
  check_debug_symbols
  echo \n--- QPainterPath::cubicTo called ---\n
  bt
  info args
  info locals
  echo \n
  continue
end

# Try to set conditional breakpoints for NaN values
# Note: These will only work if debug symbols are available and parameter names are known
python
try:
  gdb.execute("break QPainterPath::arcTo if isnan($arg1) || isnan($arg2) || isnan($arg3) || isnan($arg4) || isnan($arg5)")
  gdb.execute("commands")
  gdb.execute("  echo \\n!!! NaN DETECTED in QPainterPath::arcTo !!!\\n")
  gdb.execute("  bt full")
  gdb.execute("  info args")
  gdb.execute("  info locals")

  gdb.execute("  # Try to print which argument is NaN")
  gdb.execute("  if isnan($arg1)")
  gdb.execute("    printf \"NaN detected in x1: %f\\n\", $arg1")
  gdb.execute("  end")
  gdb.execute("  if isnan($arg2)")
  gdb.execute("    printf \"NaN detected in y1: %f\\n\", $arg2")
  gdb.execute("  end")
  gdb.execute("  if isnan($arg3)")
  gdb.execute("    printf \"NaN detected in x2: %f\\n\", $arg3")
  gdb.execute("  end")
  gdb.execute("  if isnan($arg4)")
  gdb.execute("    printf \"NaN detected in y2: %f\\n\", $arg4")
  gdb.execute("  end")
  gdb.execute("  if isnan($arg5)")
  gdb.execute("    printf \"NaN detected in angle: %f\\n\", $arg5")
  gdb.execute("  end")
  gdb.execute("  echo \\n")
  gdb.execute("end")
except:
  gdb.execute("echo Failed to set conditional breakpoint for QPainterPath::arcTo - debug symbols may be missing\\n")
end

# Try to set breakpoints for other functions that might lead to arcTo
break QPainterPath::addRoundedRect
commands
  check_debug_symbols
  echo \n--- QPainterPath::addRoundedRect called ---\n
  bt
  info args
  info locals
  echo \n
  continue
end

break QPainterPath::addEllipse
commands
  check_debug_symbols
  echo \n--- QPainterPath::addEllipse called ---\n
  bt
  info args
  info locals
  echo \n
  continue
end

# Attempt to set a breakpoint on any Qt debug/warning mechanism
python
try:
  gdb.execute("break qt_message_output")
  gdb.execute("commands")
  gdb.execute("  if $_streq(strstr($arg0, \"arcTo\"), \"arcTo\")")
  gdb.execute("    echo \\n--- QPainterPath::arcTo warning detected! ---\\n")
  gdb.execute("    printf \"Warning message: %s\\n\", $arg0")
  gdb.execute("    bt full")
  gdb.execute("  else")
  gdb.execute("    continue")
  gdb.execute("  end")
  gdb.execute("end")
except:
  gdb.execute("echo Could not set breakpoint on qt_message_output - trying alternative methods\\n")
end

# Print final messages and instructions
echo \nQPainterPath::arcTo debugging script loaded.\n
echo Breakpoints set for Qt warning mechanisms and QPainterPath functions.\n
echo Continue the application with 'continue' command.\n
echo Use 'info breakpoints' to see all active breakpoints.\n

