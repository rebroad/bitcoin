set pagination off
set logging file extract_all_samples.txt
set logging enabled on

# Function to extract samples from a QList for a specific time range
define extract_qlist_samples
    set $widget = ((TrafficGraphWidget*)0x5b0bea3d9700)
    set $time_range = $arg0

    # Get pointers to the QList data structures
    set $samples_in_data = $widget->vSamplesIn[$time_range].p.d
    set $samples_out_data = $widget->vSamplesOut[$time_range].p.d
    set $timestamps_data = $widget->vTimeStamp[$time_range].p.d

    # Print data structure info
    printf "Time Range %d: %d minutes\n", $time_range, $widget->values[$time_range]
    printf "Samples In: begin=%d, end=%d, size=%d\n", $samples_in_data->begin, $samples_in_data->end, $samples_in_data->end - $samples_in_data->begin
    printf "Samples Out: begin=%d, end=%d, size=%d\n", $samples_out_data->begin, $samples_out_data->end, $samples_out_data->end - $samples_out_data->begin
    printf "Timestamps: begin=%d, end=%d, size=%d\n", $timestamps_data->begin, $timestamps_data->end, $timestamps_data->end - $timestamps_data->begin

    # Check that the sizes match
    set $size_in = $samples_in_data->end - $samples_in_data->begin
    set $size_out = $samples_out_data->end - $samples_out_data->begin
    set $size_ts = $timestamps_data->end - $timestamps_data->begin

    # Only proceed if all sizes match and are positive
    if $size_in > 0 && $size_in == $size_out && $size_in == $size_ts
        printf "\nExtracting %d data points for range %d (%d mins)...\n", $size_in, $time_range, $widget->values[$time_range]

        # Prepare CSV header for this time range
        # Use shell command to append to CSV file
        shell echo "\n# Time Range $1: $2 minutes" >> traffic_graph_data.csv
        shell echo "index,timestamp,in_rate,out_rate" >> traffic_graph_data.csv

        # QList array is a pointer to a series of offset pointers
        # We need to extract float values from these memory locations

        # Calculate size of sample array
        set $array_size = $samples_in_data->end - $samples_in_data->begin

        # Loop through the data array
        set $i = 0
        while $i < $array_size
            set $index = $samples_in_data->begin + $i

            # Calculate actual memory index based on QList array layout
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

            # Format for CSV output
            set $csv_line = sprintf("%d,%llu,%f,%f", $i, $ts_val, $in_val, $out_val)

            # Use shell command to append to the CSV file
            shell echo "$1" >> traffic_graph_data.csv

            set $i = $i + 1
        end
    else
        printf "Error: Sample sizes don't match or are empty for range %d\n", $time_range
    end

    printf "\n"
end

# First create/initialize the output CSV file
shell echo "# TrafficGraphWidget Data Export" > traffic_graph_data2.csv
shell echo "# PID: 2760634, Extracted: $(date)" >> traffic_graph_data2.csv
shell echo "" >> traffic_graph_data2.csv

# Extract samples for all 13 time ranges
printf "Extracting TrafficGraphWidget sample data for all time ranges...\n\n"

# Use a temporary file to store data for shell processing
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

# Now, instead of using the shell command, let's prepare GDB output
# that we can manually save to CSV files

# For each time range
set $time_range = 0
while $time_range < $values_size
    # Get pointers to the QList data structures
    set $samples_in_data = $widget->vSamplesIn[$time_range].p.d
    set $samples_out_data = $widget->vSamplesOut[$time_range].p.d
    set $timestamps_data = $widget->vTimeStamp[$time_range].p.d

    # Print data structure info
    printf "Time Range %d: %d minutes\n", $time_range, $widget->values[$time_range]
    printf "Samples In: begin=%d, end=%d, size=%d\n", $samples_in_data->begin, $samples_in_data->end, $samples_in_data->end - $samples_in_data->begin
    printf "Samples Out: begin=%d, end=%d, size=%d\n", $samples_out_data->begin, $samples_out_data->end, $samples_out_data->end - $samples_out_data->begin
    printf "Timestamps: begin=%d, end=%d, size=%d\n", $timestamps_data->begin, $timestamps_data->end, $timestamps_data->end - $timestamps_data->begin

    # Check that the sizes match
    set $size_in = $samples_in_data->end - $samples_in_data->begin
    set $size_out = $samples_out_data->end - $samples_out_data->begin
    set $size_ts = $timestamps_data->end - $timestamps_data->begin

    # Only proceed if all sizes match and are positive
    if $size_in > 0 && $size_in == $size_out && $size_in == $size_ts
        printf "\nExtracting %d data points for range %d (%d mins)...\n", $size_in, $time_range, $widget->values[$time_range]
        printf "CSV DATA START - RANGE %d\n", $time_range

        # CSV header
        printf "index,timestamp,in_rate,out_rate\n"

        # Calculate size of sample array
        set $array_size = $samples_in_data->end - $samples_in_data->begin

        # Loop through all data points
        set $i = 0
        while $i < $array_size
            set $index = $samples_in_data->begin + $i

            # Get memory addresses for the values
            set $in_ptr = (void**)($samples_in_data->array + $index)
            set $out_ptr = (void**)($samples_out_data->array + $index)
            set $ts_ptr = (void**)($timestamps_data->array + $index)

            # Now extract the values
            # The float values are stored directly in the first 4 bytes of the 8-byte void* slots
            set $in_val = *(float*)$in_ptr
            set $out_val = *(float*)$out_ptr
            set $ts_val = *(long long*)$ts_ptr

            # Print as CSV line
            printf "%d,%llu,%f,%f\n", $i, $ts_val, $in_val, $out_val

            set $i = $i + 1
        end

        printf "CSV DATA END - RANGE %d\n\n", $time_range
    else
        printf "Error: Sample sizes don't match or are empty for range %d\n", $time_range
    end

    set $time_range = $time_range + 1
end

printf "\nExtraction complete! CSV data is printed between the CSV DATA START/END markers.\n"
printf "Copy each section to a separate file for each time range.\n"

set logging enabled off
quit
