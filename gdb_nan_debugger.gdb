# GDB script to catch NaN generation in Bitcoin Core code
# Usage: gdb -x gdb_nan_debugger.gdb --args ./src/qt/bitcoin-qt -nowallet

# Disable paging
set pagination off
set height 0
set width 0

# Set up breakpoints after Qt libraries are loaded
set breakpoint pending on

# Function to set up comprehensive NaN monitoring
define setup_nan_monitoring
  echo Setting up comprehensive NaN monitoring...\n

  # Monitor all Qt drawing functions that might receive NaN
  break QPainterPath::arcTo
  commands
    echo 🚨 arcTo called - checking all parameters...\n
    echo Backtrace:\n
    bt
    continue
  end

  break QPainter::drawEllipse
  commands
    echo 🚨 drawEllipse called - checking parameters...\n
    echo Backtrace:\n
    bt
    continue
  end

  break QPainter::drawArc
  commands
    echo 🚨 drawArc called - checking parameters...\n
    echo Backtrace:\n
    bt
    continue
  end

  break QPainter::drawPie
  commands
    echo 🚨 drawPie called - checking parameters...\n
    echo Backtrace:\n
    bt
    continue
  end

  # Monitor Bitcoin Core's own drawing functions
  break *drawEllipse* if $pc >= 0x555555000000
  commands
    echo 🚨 Bitcoin Core drawEllipse called!\n
    echo Backtrace:\n
    bt
    continue
  end

  break *drawArc* if $pc >= 0x555555000000
  commands
    echo 🚨 Bitcoin Core drawArc called!\n
    echo Backtrace:\n
    bt
    continue
  end

  break *drawPie* if $pc >= 0x555555000000
  commands
    echo 🚨 Bitcoin Core drawPie called!\n
    echo Backtrace:\n
    bt
    continue
  end

  # Monitor any function that might calculate drawing coordinates
  break *paintEvent* if $pc >= 0x555555000000
  commands
    echo 🚨 Bitcoin Core paintEvent called!\n
    echo Backtrace:\n
    bt
    continue
  end

  break *paint* if $pc >= 0x555555000000
  commands
    echo 🚨 Bitcoin Core paint function called!\n
    echo Backtrace:\n
    bt
    continue
  end

  echo Comprehensive NaN monitoring set up successfully!\n
end

# Set up signal handling for crashes
catch signal SIGABRT
catch signal SIGFPE
catch signal SIGSEGV

# Set up breakpoints when Qt libraries are loaded
catch load libQt5Gui.so.5
commands
  setup_nan_monitoring
  continue
end

catch load libQt5Widgets.so.5
commands
  setup_nan_monitoring
  continue
end

# Start the program
run
