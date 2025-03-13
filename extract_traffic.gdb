set pagination off
set logging file extract_traffic_data.txt
set logging enabled on

# Use a different approach to find TrafficGraphWidget instances
python
import gdb
import re

def check_widget(addr):
    try:
        widget_ptr = gdb.parse_and_eval(f"*(TrafficGraphWidget*){addr}")
        print(f"Checking potential TrafficGraphWidget at {addr}")
        
        # Try to access some members to validate if it's really a TrafficGraphWidget
        try:
            values_array = widget_ptr["values"]
            first_value = values_array[0]
            print(f"  First value in values array: {first_value}")
            
            # Check if the values match what we expect from TrafficGraphWidget
            # VALUES_SIZE should be 13 and first value should be 5
            if first_value == 5:
                print(f"  ✓ Found valid TrafficGraphWidget at {addr}")
                return widget_ptr
            else:
                print(f"  ✗ Not a valid TrafficGraphWidget (unexpected values)")
        except Exception as e:
            print(f"  ✗ Not a valid TrafficGraphWidget: {e}")
    except Exception as e:
        pass
    return None

try:
    print("Searching for TrafficGraphWidget instances...")
    
    # Try to find TrafficGraphWidget objects by scanning memory
    # First approach: Look for objects inheriting QWidget that might be TrafficGraphWidget
    
    # Get list of all C++ symbols that refer to TrafficGraphWidget methods
    symbols_output = gdb.execute("info functions TrafficGraphWidget::", to_string=True)
    print(f"TrafficGraphWidget functions found:\n{symbols_output}")
    
    # Find something that might give us the address of a TrafficGraphWidget
    # Let's look at what we can find by examining memory near the static variables
    
    # Try getting a stack trace from all threads to see if any have TrafficGraphWidget methods
    print("\nChecking thread backtraces for TrafficGraphWidget references...")
    thread_output = gdb.execute("thread apply all bt", to_string=True)
    
    # Look for potential TrafficGraphWidget addresses in the backtrace
    widget_candidates = set()
    
    # Look for any address followed by <TrafficGraphWidget::...>
    address_pattern = re.compile(r"0x([0-9a-f]+)[^<]*<TrafficGraphWidget::")
    for match in address_pattern.finditer(thread_output):
        addr = match.group(1)
        print(f"Found potential TrafficGraphWidget reference at 0x{addr}")
    
    # Another approach: Find QWidget objects and check if they're TrafficGraphWidget
    print("\nTrying to find TrafficGraphWidget by scanning memory...")
    # We know a TrafficGraphWidget uses QTimer, try to find all QTimer objects 
    # and check their parents/connections
    
    # For now, make a more direct approach
    # Let's scan for specific values that might identify a TrafficGraphWidget
    # Try these specific values from the header file in sequence: 5, 10, 20, 45, 90, ...
    value_pattern = [5, 10, 20, 45, 90, 180, 360, 720, 1440, 4320, 10080, 20160, 40320]
    
    # Let's try specific addresses from pointer dumps
    print("\nChecking specific memory locations for TrafficGraphWidget...")
    
    # We can look for vptr tables by finding all pointers to TrafficGraphWidget methods
    # Loop through memory near a known static variable
    print("Trying memory area near the static variables...")
    # Try using the static TrafficGraphWidget::staticMetaObject
    meta_addr = int(gdb.parse_and_eval("&TrafficGraphWidget::staticMetaObject"))
    print(f"staticMetaObject address: 0x{meta_addr:x}")
    
    # Try to find any pointers to TrafficGraphWidget objects using find memory command
    # We need to find a pattern that's likely to be in the TrafficGraphWidget
    # Try looking for vptr pointing to TrafficGraphWidget's virtual table
    
    # Let's try a more direct approach - dump pointers from various memory regions
    # We know TrafficGraphWidget extends QWidget, so it should have a vtable pointer at the start
    # Find all Qt widgets using a specific QObject inheritance check
    
    # Find objects by looking at global symbols
    print("\nChecking for TrafficGraphWidget in global symbols...")
    symbols = gdb.execute("info variables ^[^:]", to_string=True)
    
    # Let's try a more specific approach by looking at QObject signal-slot connections
    # TrafficGraphWidget has a timer, so look for timer connections
    
    # Direct approach - check specific address
    print("\nManually check a specific address that might be the TrafficGraphWidget...")
    
    # Try the previously found TrafficGraphWidget instance address
    potential_widgets = [
        "0x5b0bea3d9700"  # From the previous extract attempt
    ]
    
    for addr in potential_widgets:
        widget = check_widget(addr)
        if widget:
            # We found a legitimate TrafficGraphWidget - now extract data
            
            print("\n=== TrafficGraphWidget found! Extracting data... ===")
            values_size = 13  # From the header file
            
            # Extract values array
            values = []
            for i in range(values_size):
                values.append(int(widget["values"][i]))
            print(f"Values array: {values}")
            
            # Prepare data file
            with open('traffic_data_dump.txt', 'w') as f:
                f.write("# TrafficGraphWidget data extracted from PID 2760634\n\n")
                
                # Export values array
                f.write(f"values = {values}\n\n")
                
                # Export nLastBytesIn
                last_bytes_in = []
                for i in range(values_size):
                    last_bytes_in.append(int(widget["nLastBytesIn"][i]))
                f.write(f"nLastBytesIn = {last_bytes_in}\n\n")
                
                # Export nLastBytesOut
                last_bytes_out = []
                for i in range(values_size):
                    last_bytes_out.append(int(widget["nLastBytesOut"][i]))
                f.write(f"nLastBytesOut = {last_bytes_out}\n\n")
                
                # Export nLastTime
                last_time = []
                for i in range(values_size):
                    last_time.append(int(widget["nLastTime"][i]["__r"]))
                f.write(f"nLastTime = {last_time}\n\n")
                
                # Export samples from QQueues
                f.write("# Sample data from QQueues\n")
                f.write("# Format: [index, timestamp, in_rate, out_rate]\n")
                
                all_samples = []
                for i in range(values_size):
                    samples = []
                    f.write(f"\n# Time range {i}: {values[i]} minutes\n")
                    try:
                        # Try to access queue data (this is tricky without debug symbols)
                        in_queue = widget["vSamplesIn"][i]
                        out_queue = widget["vSamplesOut"][i]
                        ts_queue = widget["vTimeStamp"][i]
                        
                        # Different Qt versions store QQueue data differently
                        # Try several known layouts
                        try:
                            # First try standard Qt5 layout with direct d pointer
                            size = in_queue["d"]["size"]
                            print(f"Queue size for time range {i}: {size}")
                            
                            for j in range(size):
                                in_val = float(in_queue["d"]["array"][j])
                                out_val = float(out_queue["d"]["array"][j])
                                ts_val = int(ts_queue["d"]["array"][j]["__r"])
                                samples.append([j, ts_val, in_val, out_val])
                        except:
                            # Try Qt5 layout with _q_d
                            try:
                                size = in_queue["_q_d"]["d"]["size"]
                                print(f"Queue size for time range {i}: {size}")
                                
                                for j in range(size):
                                    in_val = float(in_queue["_q_d"]["d"]["array"][j])
                                    out_val = float(out_queue["_q_d"]["d"]["array"][j])
                                    ts_val = int(ts_queue["_q_d"]["d"]["array"][j]["__r"])
                                    samples.append([j, ts_val, in_val, out_val])
                            except:
                                # Last attempt with implicitly shared d-pointer
                                try:
                                    # Generic attempt to examine the structure
                                    print(f"Queue structure: {in_queue}")
                                    print("Could not determine queue layout - please examine manually")
                                except Exception as e:
                                    print(f"Error examining queue: {e}")
                    except Exception as e:
                        print(f"Error accessing queue for time range {i}: {e}")
                        
                    # Write samples to file
                    for sample in samples:
                        f.write(f"sample_{i}_{sample[0]} = {sample}\n")
                    
                    all_samples.append(samples)
            
            print("\nData exported to traffic_data_dump.txt")
            print("You may need to manually customize the extraction based on the specific memory layout")
            
    if not widget_candidates:
        print("\nCould not find TrafficGraphWidget instance automatically.")
        print("Try manual inspection or compiling with debug symbols.")
except Exception as e:
    print(f"Error in Python script: {e}")
end

set logging enabled off
quit
