set pagination off
set logging file extract_all_samples.txt
set logging redirect on
set logging enabled on

# Extract samples for all 13 time ranges
printf "Extracting TrafficGraphWidget sample data for all time ranges...\n\n"

# First, suspend all threads to minimize concurrent modification issues
thread apply all continue&

# The address below was found using:-
# (gdb) info all-objects TrafficGraphWidget

set $widget = ((TrafficGraphWidget*)0x5b0bea3d9700)

# Extract values array (time ranges)
set $values_size = 13
shell mv traffic_graph_data.csv traffic_graph_data.csv.old
shell echo "# TrafficGraphWidget Data Export" > traffic_graph_data.csv

# Extract all values arrays first
printf "Values array: ["
set $cmd = "echo \"# Values array: ["
set $i = 0
while $i < $values_size
    printf "%d", $widget->values[$i]
    set $cmd = sprintf("%s%d", $cmd, $widget->values[$i])
    if $i < $values_size - 1
        printf ", "
        set $cmd = sprintf("%s, ", $cmd)
    end
    set $i = $i + 1
end
printf "]\n\n"
set $cmd = sprintf("%s]\" >> traffic_graph_data.csv", $cmd)
shell $cmd
shell echo "" >> traffic_graph_data.csv

# First just extract basic info for each time range - diagnose issues
printf "Examining array info for each time range...\n"
set $time_range = 0
while $time_range < $values_size
    # Get pointers to the QList data structures - read once, copy to local vars
    set $samples_in_data = $widget->vSamplesIn[$time_range].p.d
    set $samples_out_data = $widget->vSamplesOut[$time_range].p.d
    set $timestamps_data = $widget->vTimeStamp[$time_range].p.d
    set $minutes = $widget->values[$time_range]

    # Check if pointers are valid
    if $samples_in_data != 0 && $samples_out_data != 0 && $timestamps_data != 0
        # Read array metadata
        set $begin_in = $samples_in_data->begin
        set $end_in = $samples_in_data->end
        set $begin_out = $samples_out_data->begin
        set $end_out = $samples_out_data->end
        set $begin_ts = $timestamps_data->begin
        set $end_ts = $timestamps_data->end

        # Calculate sizes
        set $size_in = $end_in - $begin_in
        set $size_out = $end_out - $begin_out
        set $size_ts = $end_ts - $begin_ts

        # Print summary
        printf "Time Range %d: %d minutes\n", $time_range, $minutes
        printf "  Samples In: begin=%d, end=%d, size=%d\n", $begin_in, $end_in, $size_in
        printf "  Samples Out: begin=%d, end=%d, size=%d\n", $begin_out, $end_out, $size_out
        printf "  Timestamps: begin=%d, end=%d, size=%d\n", $begin_ts, $end_ts, $size_ts

        # Add this info to CSV
        set $cmd = sprintf("echo \"# Time Range %d: %d minutes (size=%d)\" >> traffic_graph_data.csv", $time_range, $minutes, $size_in)
        shell $cmd
    else
        printf "Time Range %d: Invalid pointers\n", $time_range
    end

    set $time_range = $time_range + 1
end

printf "\nTime range analysis complete. Starting data extraction...\n"
shell echo "" >> traffic_graph_data.csv

# Now extract data for each range more carefully
set $time_range = 0
while $time_range < $values_size
    # Get pointers to the QList data structures - read once into local vars
    set $samples_in_data = $widget->vSamplesIn[$time_range].p.d
    set $samples_out_data = $widget->vSamplesOut[$time_range].p.d
    set $timestamps_data = $widget->vTimeStamp[$time_range].p.d
    set $minutes = $widget->values[$time_range]

    # Check if pointers are valid
    if $samples_in_data != 0 && $samples_out_data != 0 && $timestamps_data != 0
        # Read array metadata
        set $begin_in = $samples_in_data->begin
        set $end_in = $samples_in_data->end
        set $begin_out = $samples_out_data->begin
        set $end_out = $samples_out_data->end
        set $begin_ts = $timestamps_data->begin
        set $end_ts = $timestamps_data->end

        # Calculate sizes
        set $size_in = $end_in - $begin_in
        set $size_out = $end_out - $begin_out
        set $size_ts = $end_ts - $begin_ts

        # Sanity check the sizes before proceeding
        if $size_in > 0 && $size_in < 5000 && $size_in == $size_out && $size_in == $size_ts
            printf "\nExtracting %d data points for range %d (%d mins)...\n", $size_in, $time_range, $minutes

            # Add header for this time range to the CSV file
            shell echo "" >> traffic_graph_data.csv
            set $cmd = sprintf("echo \"# Time Range %d: %d minutes\" >> traffic_graph_data.csv", $time_range, $minutes)
            shell $cmd
            shell echo "index,timestamp,in_rate,out_rate" >> traffic_graph_data.csv

            # Process in smaller batches of 10 samples
            set $batch_size = 10
            set $current_batch = 0
            set $total_batches = ($size_in + $batch_size - 1) / $batch_size

            while $current_batch < $total_batches
                set $start_idx = $current_batch * $batch_size
                set $end_idx = $start_idx + $batch_size
                if $end_idx > $size_in
                    set $end_idx = $size_in
                end

                printf "  Processing batch %d/%d (samples %d-%d)...\n", $current_batch+1, $total_batches, $start_idx, $end_idx-1

                # Process each sample in the batch
                set $i = $start_idx
                while $i < $end_idx
                    set $index = $begin_in + $i

                    # Use a safer memory reading approach
                    # First check if the array pointers are valid
                    set $array_in = $samples_in_data->array
                    set $array_out = $samples_out_data->array
                    set $array_ts = $timestamps_data->array

                    if $array_in != 0 && $array_out != 0 && $array_ts != 0
                        # Read raw memory values first into variables
                        # This is safer than direct pointer dereferencing
                        set $mem_in = (unsigned long long)(*($array_in + $index))
                        set $mem_out = (unsigned long long)(*($array_out + $index))
                        set $mem_ts = (unsigned long long)(*($array_ts + $index))

                        # Now convert to appropriate types
                        # Cast the memory to the appropriate type
                        set $in_val = *(float*)&$mem_in
                        set $out_val = *(float*)&$mem_out
                        set $ts_val = $mem_ts

                        # Format CSV line
                        set $cmd = sprintf("echo \"%d,%llu,%f,%f\" >> traffic_graph_data.csv", $i, $ts_val, $in_val, $out_val)
                        shell $cmd
                    else
                        printf "  Warning: Invalid array pointers at index %d\n", $i
                        break
                    end

                    set $i = $i + 1
                end

                set $current_batch = $current_batch + 1
            end

            printf "Completed extraction for time range %d\n", $time_range
        else
            printf "Skipping time range %d: Sample sizes don't match or are invalid: %d, %d, %d\n", $time_range, $size_in, $size_out, $size_ts
            set $cmd = sprintf("echo \"# Skipped time range %d: Sample sizes don't match or are invalid\" >> traffic_graph_data.csv", $time_range)
            shell $cmd
        end
    else
        printf "Skipping time range %d: Invalid pointers\n", $time_range
        set $cmd = sprintf("echo \"# Skipped time range %d: Invalid pointers\" >> traffic_graph_data.csv", $time_range)
        shell $cmd
    end

    set $time_range = $time_range + 1
end

printf "\nExtraction complete! Data saved to traffic_graph_data.csv\n"

set logging enabled off
quit
