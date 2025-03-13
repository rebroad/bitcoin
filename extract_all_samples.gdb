set pagination off
set logging file extract_all_samples.txt
set logging redirect on
set logging enabled on

# Extract samples for all 13 time ranges
printf "Extracting TrafficGraphWidget sample data for all time ranges...\n\n"

set $values_size = 13
set $widget = ((TrafficGraphWidget*)0x5b0bea3d9700)

# Extract all values arrays first
printf "Values array: ["
set $i = 0
while $i < $values_size
    printf "%d", $widget->values[$i]
    if $i < $values_size - 1
        printf ", "
    end
    set $i = $i + 1
end
printf "]\n\n"

# First create/initialize the output CSV file
shell rm -f traffic_graph_data.csv
shell echo "# TrafficGraphWidget Data Export" > traffic_graph_data.csv
shell echo "# PID: 2760634, Extracted: $(date)" >> traffic_graph_data.csv
shell echo "# Values array: [5, 10, 20, 45, 90, 180, 360, 720, 1440, 4320, 10080, 20160, 40320]" >> traffic_graph_data.csv
shell echo "" >> traffic_graph_data.csv

# For each time range
set $time_range = 0
while $time_range < $values_size
    # Get pointers to the QList data structures
    set $samples_in_data = $widget->vSamplesIn[$time_range].p.d
    set $samples_out_data = $widget->vSamplesOut[$time_range].p.d
    set $timestamps_data = $widget->vTimeStamp[$time_range].p.d

    # Get time range minutes for header
    set $minutes = $widget->values[$time_range]

    # Print data structure info
    printf "Time Range %d: %d minutes\n", $time_range, $minutes
    printf "Samples In: begin=%d, end=%d, size=%d\n", $samples_in_data->begin, $samples_in_data->end, $samples_in_data->end - $samples_in_data->begin
    printf "Samples Out: begin=%d, end=%d, size=%d\n", $samples_out_data->begin, $samples_out_data->end, $samples_out_data->end - $samples_out_data->begin
    printf "Timestamps: begin=%d, end=%d, size=%d\n", $timestamps_data->begin, $timestamps_data->end, $timestamps_data->end - $timestamps_data->begin

    # Check that the sizes match
    set $size_in = $samples_in_data->end - $samples_in_data->begin
    set $size_out = $samples_out_data->end - $samples_out_data->begin
    set $size_ts = $timestamps_data->end - $timestamps_data->begin

    # Only proceed if all sizes match and are positive
    if $size_in > 0 && $size_in == $size_out && $size_in == $size_ts
        printf "\nExtracting %d data points for range %d (%d mins)...\n", $size_in, $time_range, $minutes

        # Add header for this time range to the CSV file
        # Use gdb to format the command including the variables
        shell echo "" >> traffic_graph_data.csv
        set $cmd = sprintf("echo \"# Time Range %d: %d minutes\" >> traffic_graph_data.csv", $time_range, $minutes)
        shell $cmd
        shell echo "index,timestamp,in_rate,out_rate" >> traffic_graph_data.csv

        # Calculate size of sample array
        set $array_size = $samples_in_data->end - $samples_in_data->begin

        # Loop through the data array
        set $i = 0
        while $i < $array_size
            set $index = $samples_in_data->begin + $i

            # In Qt's QList, the actual data is stored in this array of void*
            # where each pointer-sized slot contains the actual data

            # Access data using direct memory reads - this is platform dependent
            # Pointers in the array don't point to float values, they contain the float values
            # For a 64-bit platform, we need to:
            # 1. Get memory address of the array element
            # 2. Extract the float value which is in the first 4 bytes of the 8-byte pointer

            # Get memory addresses for the values
            set $in_ptr = (void**)($samples_in_data->array + $index)
            set $out_ptr = (void**)($samples_out_data->array + $index)
            set $ts_ptr = (void**)($timestamps_data->array + $index)

            # Now extract the values
            # The float values are stored directly in the first 4 bytes of the 8-byte void* slots
            set $in_val = *(float*)$in_ptr
            set $out_val = *(float*)$out_ptr
            set $ts_val = *(long long*)$ts_ptr

            # Format string for CSV output using GDB's printf
            set $cmd = sprintf("echo \"%d,%llu,%f,%f\" >> traffic_graph_data.csv", $i, $ts_val, $in_val, $out_val)
            shell $cmd

            set $i = $i + 1

            # Print progress every 100 samples
            if $i % 100 == 0
                printf "  Processed %d/%d samples\n", $i, $array_size
            end
        end

        printf "Completed extraction for time range %d\n", $time_range
    else
        printf "Error: Sample sizes don't match or are empty for range %d\n", $time_range
    end

    set $time_range = $time_range + 1
end

printf "\nExtraction complete! Data saved to traffic_graph_data.csv\n"

set logging enabled off
quit
