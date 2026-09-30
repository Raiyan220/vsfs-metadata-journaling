#!/bin/bash
echo "Resetting disk..."
./mkfs
echo "Starting stress test (creating 20 files without install)..."
for i in {1..20}
do
    echo "Attempting to create file_$i.txt"
    ./journal create "file_$i.txt"
    if [ $? -ne 0 ]; then
        echo "SUCCESS: Journal stopped at file $i"
        break
    fi
done
